#pragma once

/// \file dispatch/broker_reversal_reconciliation.hpp
/// \brief Builds and durably commits reversal evidence from graph samples.

#include "broker_reversal.hpp"

#include <mt5bridge/reconciliation/coordinator.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace mt5bridge {

/// \struct BrokerVolumeNormalization
/// \brief Exact decimal scale and symbol step used at the observation boundary.
struct BrokerVolumeNormalization {
    std::uint32_t scale = 0; ///< Decimal denominator exponent, at most nine.
    std::uint64_t step_units = 0; ///< Positive symbol step at that scale.

    /// \brief Tests whether the normalization domain is usable.
    /// \return True when the scale and step are representable.
    bool valid() const { return scale <= 9 && step_units != 0; }
};

/// \brief Converts one broker floating-point volume into exact step units.
/// \param raw Volume copied from an MT5 snapshot.
/// \param normalization Decimal scale and symbol step proof.
/// \return Exact step-normalized volume, or empty when conversion is ambiguous.
inline std::optional<BrokerVolume> normalize_broker_volume(
    double raw, const BrokerVolumeNormalization &normalization) {
    if (!normalization.valid() || !std::isfinite(raw) || raw <= 0.0)
        return std::nullopt;

    long double multiplier = 1.0L;
    for (std::uint32_t index = 0; index != normalization.scale; ++index)
        multiplier *= 10.0L;
    const long double scaled = static_cast<long double>(raw) * multiplier;
    if (!std::isfinite(scaled) || scaled <= 0.0L)
        return std::nullopt;
    const long double rounded = std::round(scaled);
    const long double tolerance =
        1.0e-9L * ((std::abs(scaled) > 1.0L) ? std::abs(scaled) : 1.0L);
    if (!std::isfinite(rounded) || std::abs(scaled - rounded) > tolerance ||
        rounded > static_cast<long double>((std::numeric_limits<std::uint64_t>::max)()))
        return std::nullopt;

    const auto units = static_cast<std::uint64_t>(rounded);
    BrokerVolume result{units, normalization.scale, normalization.step_units};
    return result.valid() ? std::optional<BrokerVolume>(result) : std::nullopt;
}

/// \struct BrokerReversalObservationRequest
/// \brief Selects one deal and the exact normalization proof for its samples.
struct BrokerReversalObservationRequest {
    std::uint64_t deal_ticket = 0; ///< DEAL_TICKET expected in the post sample.
    BrokerMarginMode margin_mode = BrokerMarginMode::retail_netting;
    ObservationWindow history_window; ///< Post-action history query window.
    BrokerVolumeNormalization volume;

    /// \brief Tests whether the request can produce broker evidence.
    /// \return True when deal identity, netting mode, and volume proof exist.
    bool valid() const {
        return deal_ticket != 0 && broker_margin_mode_is_netting(margin_mode) &&
               history_window.valid() && volume.valid();
    }
};

/// \enum BrokerReversalReconcileStatus
/// \brief Reports graph admission and durable broker-record outcomes.
enum class BrokerReversalReconcileStatus {
    committed,         ///< A new broker record was durably written.
    already_committed, ///< The exact broker record was already present.
    conflict,          ///< The deal key has a different durable record.
    invalid_observation, ///< Samples do not prove one causal reversal.
    invalid_record,    ///< Derived evidence failed durable validation.
    io_error,          ///< Durable storage could not complete the operation.
};

/// \struct BrokerReversalReconcileResult
/// \brief Carries the durable result and the candidate broker record.
struct BrokerReversalReconcileResult {
    BrokerReversalReconcileStatus status =
        BrokerReversalReconcileStatus::invalid_observation;
    std::optional<BrokerReversalRecord> record;

    /// \brief Tests whether a record was accepted by durable storage.
    /// \return True for a new commit or an identical replay.
    bool accepted() const {
        return status == BrokerReversalReconcileStatus::committed ||
               status == BrokerReversalReconcileStatus::already_committed;
    }
};

namespace detail {

inline std::optional<std::string> broker_reversal_snapshot_text(
    const char *value, std::size_t capacity) {
    const auto *terminator = static_cast<const char *>(std::memchr(value, '\0', capacity));
    if (!terminator)
        return std::nullopt;
    return std::string(value, static_cast<std::size_t>(terminator - value));
}

inline std::optional<BrokerPositionDirection> broker_reversal_direction(
    std::uint32_t type) {
    // MT5 DEAL_TYPE_BUY/POSITION_TYPE_BUY are 0; SELL values are 1.
    if (type == 0)
        return BrokerPositionDirection::buy;
    if (type == 1)
        return BrokerPositionDirection::sell;
    return std::nullopt;
}

class BrokerReversalDigest {
public:
    void add_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            value_ ^= static_cast<std::uint8_t>(value >> shift);
            value_ *= 1099511628211ULL;
        }
    }

    void add_double(double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
        std::memcpy(&bits, &value, sizeof(bits));
        add_u64(bits);
    }

    void add_text(const std::string &value) {
        for (const unsigned char byte : value) {
            value_ ^= byte;
            value_ *= 1099511628211ULL;
        }
        add_u64(static_cast<std::uint64_t>(value.size()));
    }

    std::uint64_t value() const { return value_ == 0 ? 1 : value_; }

private:
    std::uint64_t value_ = 1469598103934665603ULL;
};

inline void add_position_digest(BrokerReversalDigest *digest,
                                const Mt5PositionSnapshot &position) {
    digest->add_u64(position.ticket);
    digest->add_u64(position.identifier);
    digest->add_u64(position.known_fields);
    digest->add_u64(position.type);
    digest->add_double(position.volume);
    if (const auto symbol = broker_reversal_snapshot_text(position.symbol,
                                                           sizeof(position.symbol)))
        digest->add_text(*symbol);
}

inline void add_deal_digest(BrokerReversalDigest *digest,
                            const Mt5DealSnapshot &deal) {
    digest->add_u64(deal.ticket);
    digest->add_u64(deal.order_ticket);
    digest->add_u64(deal.position_id);
    digest->add_u64(deal.known_fields);
    digest->add_u64(deal.type);
    digest->add_u64(deal.entry);
    digest->add_double(deal.volume);
    digest->add_u64(static_cast<std::uint64_t>(deal.time_msc));
    if (const auto symbol = broker_reversal_snapshot_text(deal.symbol,
                                                           sizeof(deal.symbol)))
        digest->add_text(*symbol);
}

inline std::optional<Mt5PositionSnapshot> find_reversal_position(
    const ObservationBatch &batch, std::uint64_t identifier) {
    if (!observes(batch.observed_domains, ObservationDomain::positions) ||
        identifier == 0)
        return std::nullopt;
    std::optional<Mt5PositionSnapshot> result;
    for (const auto &position : batch.positions) {
        if (position.identifier != identifier)
            continue;
        if (result.has_value())
            return std::nullopt;
        result = position;
    }
    return result;
}

inline std::optional<Mt5DealSnapshot> find_reversal_deal(
    const ObservationBatch &batch, std::uint64_t ticket) {
    if (!observes(batch.observed_domains, ObservationDomain::history_deals) ||
        ticket == 0)
        return std::nullopt;
    std::optional<Mt5DealSnapshot> result;
    for (const auto &deal : batch.history_deals) {
        if (deal.ticket != ticket)
            continue;
        if (result.has_value())
            return std::nullopt;
        result = deal;
    }
    return result;
}

inline bool reversal_deal_absent(
    const ObservationBatch &batch, std::uint64_t ticket) {
    if (!observes(batch.observed_domains, ObservationDomain::history_deals) ||
        ticket == 0)
        return false;
    for (const auto &deal : batch.history_deals) {
        if (deal.ticket == ticket)
            return false;
    }
    return true;
}

} // namespace detail

/// \brief Derives exact broker evidence from one causal pre/post sample pair.
/// \param graph Graph that admitted both samples.
/// \param pre Sample captured before the broker reversal.
/// \param post Sample captured after the reversal and containing its deal.
/// \param request Deal identity, account mode, and symbol-volume proof.
/// \return A normalized observation, or empty when provenance is ambiguous.
inline std::optional<BrokerReversalObservation> derive_broker_reversal_observation(
    const ObservationGraph &graph, const ObservationSample &pre,
    const ObservationSample &post, const BrokerReversalObservationRequest &request) {
    if (!request.valid() || !graph.bound() ||
        pre.graph_instance_id() != graph.instance_id() ||
        post.graph_instance_id() != graph.instance_id() ||
        pre.graph_instance_id() != post.graph_instance_id() ||
        pre.graph_revision() == 0 || post.graph_revision() <= pre.graph_revision() ||
        post.graph_revision() > graph.revision() ||
        pre.batch().account != graph.account_key() ||
        post.batch().account != graph.account_key() ||
        !observes(pre.batch().observed_domains, ObservationDomain::positions) ||
        !observes(pre.batch().observed_domains, ObservationDomain::history_deals) ||
        !observes(post.batch().observed_domains, ObservationDomain::positions) ||
        !observes(post.batch().observed_domains, ObservationDomain::history_deals) ||
        pre.batch().position_history_order !=
            PositionHistoryOrder::positions_before_history_deals ||
        post.batch().position_history_order !=
            PositionHistoryOrder::history_deals_before_positions ||
        !pre.batch().history_deals_window ||
        !pre.batch().history_deals_window->valid() ||
        pre.batch().history_deals_window->from_msc != request.history_window.from_msc ||
        pre.batch().history_deals_window->to_msc != request.history_window.to_msc ||
        !post.batch().history_deals_window ||
        !post.batch().history_deals_window->valid() ||
        post.batch().history_deals_window->from_msc != request.history_window.from_msc ||
        post.batch().history_deals_window->to_msc != request.history_window.to_msc ||
        !graph.history_deals_covered_at(request.history_window,
                                        post.graph_revision()))
        return std::nullopt;

    if (!detail::reversal_deal_absent(pre.batch(), request.deal_ticket))
        return std::nullopt;

    constexpr std::uint64_t kRequiredPositionFields =
        MT5BRIDGE_POSITION_KNOWN_TICKET | MT5BRIDGE_POSITION_KNOWN_IDENTIFIER |
        MT5BRIDGE_POSITION_KNOWN_TYPE | MT5BRIDGE_POSITION_KNOWN_VOLUME |
        MT5BRIDGE_POSITION_KNOWN_SYMBOL;
    constexpr std::uint64_t kRequiredDealFields =
        MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
        MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
        MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME |
        MT5BRIDGE_DEAL_KNOWN_SYMBOL;

    const auto deal = detail::find_reversal_deal(post.batch(), request.deal_ticket);
    if (!deal || (deal->known_fields & kRequiredDealFields) != kRequiredDealFields ||
        deal->position_id == 0 || deal->order_ticket == 0 ||
        deal->entry != static_cast<std::uint32_t>(BrokerDealEntry::inout) ||
        deal->time_msc < request.history_window.from_msc ||
        deal->time_msc > request.history_window.to_msc)
        return std::nullopt;

    const auto pre_position =
        detail::find_reversal_position(pre.batch(), deal->position_id);
    const auto post_position =
        detail::find_reversal_position(post.batch(), deal->position_id);
    if (!pre_position || !post_position ||
        (pre_position->known_fields & kRequiredPositionFields) !=
            kRequiredPositionFields ||
        (post_position->known_fields & kRequiredPositionFields) !=
            kRequiredPositionFields)
        return std::nullopt;

    const auto deal_symbol = detail::broker_reversal_snapshot_text(
        deal->symbol, sizeof(deal->symbol));
    const auto pre_symbol = detail::broker_reversal_snapshot_text(
        pre_position->symbol, sizeof(pre_position->symbol));
    const auto post_symbol = detail::broker_reversal_snapshot_text(
        post_position->symbol, sizeof(post_position->symbol));
    if (!deal_symbol || !pre_symbol || !post_symbol || deal_symbol->empty() ||
        *deal_symbol != *pre_symbol || *deal_symbol != *post_symbol)
        return std::nullopt;

    const auto pre_direction = detail::broker_reversal_direction(pre_position->type);
    const auto post_direction = detail::broker_reversal_direction(post_position->type);
    const auto deal_direction = detail::broker_reversal_direction(deal->type);
    const auto pre_volume =
        normalize_broker_volume(pre_position->volume, request.volume);
    const auto post_volume =
        normalize_broker_volume(post_position->volume, request.volume);
    const auto deal_volume = normalize_broker_volume(deal->volume, request.volume);
    if (!pre_direction || !post_direction || !deal_direction || !pre_volume ||
        !post_volume || !deal_volume)
        return std::nullopt;

    BrokerReversalObservation observation;
    observation.account = post.batch().account;
    observation.margin_mode = request.margin_mode;
    observation.symbol = *deal_symbol;
    observation.deal_ticket = deal->ticket;
    observation.order_ticket = deal->order_ticket;
    observation.deal_position_id = deal->position_id;
    observation.deal_entry = BrokerDealEntry::inout;
    observation.pre_position_identifier = pre_position->identifier;
    observation.pre_direction = *pre_direction;
    observation.pre_volume = *pre_volume;
    observation.post_position_identifier = post_position->identifier;
    observation.post_direction = *post_direction;
    observation.post_volume = *post_volume;
    observation.deal_direction = *deal_direction;
    observation.deal_volume = *deal_volume;
    observation.provenance.graph_instance_id = pre.graph_instance_id();
    observation.provenance.pre_graph_revision = pre.graph_revision();
    observation.provenance.post_graph_revision = post.graph_revision();
    observation.provenance.pre_positions_revision = pre.graph_revision();
    observation.provenance.post_positions_revision = post.graph_revision();
    observation.provenance.history_deals_revision = post.graph_revision();

    detail::BrokerReversalDigest digest;
    digest.add_u64(pre.graph_revision());
    digest.add_u64(post.graph_revision());
    digest.add_u64(static_cast<std::uint64_t>(pre.batch().position_history_order));
    digest.add_u64(static_cast<std::uint64_t>(post.batch().position_history_order));
    digest.add_u64(request.history_window.from_msc);
    digest.add_u64(request.history_window.to_msc);
    detail::add_position_digest(&digest, *pre_position);
    detail::add_position_digest(&digest, *post_position);
    detail::add_deal_digest(&digest, *deal);
    observation.provenance.evidence_digest = digest.value();
    return observation;
}

/// \brief Derives and durably commits one causal broker reversal.
/// \param graph Graph that admitted both samples.
/// \param pre Sample captured before the reversal.
/// \param post Sample captured after the reversal.
/// \param request Deal and exact volume proof to reconcile.
/// \param store Durable broker-record store receiving the immutable fact.
/// \return Validation, idempotency, conflict, or I/O outcome.
inline BrokerReversalReconcileResult reconcile_broker_reversal(
    const ObservationGraph &graph, const ObservationSample &pre,
    const ObservationSample &post, const BrokerReversalObservationRequest &request,
    DurableBrokerReversalStore &store) {
    const auto observation =
        derive_broker_reversal_observation(graph, pre, post, request);
    if (!observation)
        return {BrokerReversalReconcileStatus::invalid_observation, std::nullopt};
    const auto record = derive_broker_reversal_record(*observation);
    if (!record)
        return {BrokerReversalReconcileStatus::invalid_record, std::nullopt};

    const auto commit_status = store.commit(*record);
    BrokerReversalReconcileResult result;
    result.record = record;
    switch (commit_status) {
    case BrokerReversalCommitStatus::committed:
        result.status = BrokerReversalReconcileStatus::committed;
        break;
    case BrokerReversalCommitStatus::already_committed:
        result.status = BrokerReversalReconcileStatus::already_committed;
        break;
    case BrokerReversalCommitStatus::conflict:
        result.status = BrokerReversalReconcileStatus::conflict;
        break;
    case BrokerReversalCommitStatus::invalid_record:
        result.status = BrokerReversalReconcileStatus::invalid_record;
        break;
    case BrokerReversalCommitStatus::io_error:
        result.status = BrokerReversalReconcileStatus::io_error;
        break;
    }
    return result;
}

} // namespace mt5bridge

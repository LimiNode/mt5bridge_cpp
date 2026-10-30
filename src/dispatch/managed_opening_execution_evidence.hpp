#pragma once

/// \file dispatch/managed_opening_execution_evidence.hpp
/// \brief Defines locally validated OPEN execution facts and cumulative frontiers.

#include "managed_opening_quantity_contract.hpp"

#include <mt5bridge/dispatch/broker_reversal.hpp>
#include <mt5bridge/dispatch/journal_types.hpp>
#include <mt5bridge/trade/deals.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {

/// \brief Maximum canonical byte size accepted for one durable OPEN fact.
inline constexpr std::size_t kManagedOpeningMaxCanonicalBytes = 1U << 20;

/// \struct ManagedOpeningExecutionRequest
/// \brief Constrains a local OPEN deal join without supplying source authority.
struct ManagedOpeningExecutionRequest {
    OperationKey operation_key; ///< Expected account-scoped OPEN operation.
    OperationKind operation_kind = OperationKind::open; ///< Expected operation kind.
    /// Immutable revision at which the OPEN broker result became durable.
    std::uint64_t operation_revision = 0;
    std::uint64_t order_ticket = 0; ///< Result-derived OPEN order ticket.
    std::uint64_t position_identifier = 0; ///< Expected broker position identity.

    /// \brief Tests whether all join constraints are usable.
    /// \return True for a complete OPEN query.
    bool valid() const {
        return operation_key.valid() && operation_kind == OperationKind::open &&
               operation_revision != 0 && order_ticket != 0 &&
               position_identifier != 0;
    }
};

/// \struct ManagedOpeningDealObservation
/// \brief Candidate history facts that still require authoritative source reload.
///
/// This value is only data. A future producer must independently obtain it
/// from the account-scoped journal and observation graph under the lease.
struct ManagedOpeningDealObservation {
    OperationKey operation_key; ///< Operation associated with the observed order.
    /// Immutable result-source revision used for the journal join.
    std::uint64_t operation_revision = 0;
    std::uint64_t order_ticket = 0; ///< DEAL_ORDER identity.
    std::uint64_t deal_ticket = 0; ///< DEAL_TICKET identity.
    std::uint64_t position_identifier = 0; ///< DEAL_POSITION_ID identity.
    std::uint64_t known_fields = 0; ///< Fields present in the history record.
    BrokerPositionDirection direction = BrokerPositionDirection::unknown;
    BrokerDealEntry entry = BrokerDealEntry::unknown;
    std::int64_t deal_time_msc = 0; ///< DEAL_TIME in milliseconds.
    double observed_volume = 0.0; ///< Raw broker volume from this deal.
    double observed_step = 0.0; ///< Reviewed broker step observed for the symbol.
    ManagedOpeningQuantityDomain quantity_domain; ///< Reviewed B2 quantity mapping.
    std::uint64_t source_revision = 0; ///< Revision that admitted this deal.
    std::uint64_t graph_instance_id = 0; ///< Process-local source instance.
    std::uint64_t history_coverage_revision = 0; ///< Complete-window revision.
    std::int64_t history_window_from_msc = 0; ///< Inclusive coverage start.
    std::int64_t history_window_to_msc = 0; ///< Inclusive coverage end.
};

/// \brief Deal fields required for a locally complete OPEN execution fact.
inline constexpr std::uint64_t kManagedOpeningRequiredDealFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TYPE |
    MT5BRIDGE_DEAL_KNOWN_ENTRY | MT5BRIDGE_DEAL_KNOWN_VOLUME |
    MT5BRIDGE_DEAL_KNOWN_TIME;

/// \enum ManagedOpeningExecutionValidationStatus
/// \brief Reports local join and observation-shape validation.
enum class ManagedOpeningExecutionValidationStatus : std::uint8_t {
    valid = 0,
    invalid_request = 1,
    operation_mismatch = 2,
    order_mismatch = 3,
    position_mismatch = 4,
    incomplete_history = 5,
    invalid_deal = 6,
    invalid_quantity = 7,
};

/// \brief Checks candidate fields without claiming journal or broker authority.
/// \param request Expected committed OPEN identity.
/// \param observation Candidate history observation.
/// \return Local validation status.
inline ManagedOpeningExecutionValidationStatus validate_managed_opening_observation(
    const ManagedOpeningExecutionRequest &request,
    const ManagedOpeningDealObservation &observation) {
    if (!request.valid())
        return ManagedOpeningExecutionValidationStatus::invalid_request;
    if (observation.operation_key != request.operation_key ||
        observation.operation_revision != request.operation_revision)
        return ManagedOpeningExecutionValidationStatus::operation_mismatch;
    if (observation.order_ticket != request.order_ticket)
        return ManagedOpeningExecutionValidationStatus::order_mismatch;
    if (observation.position_identifier != request.position_identifier)
        return ManagedOpeningExecutionValidationStatus::position_mismatch;
    // ADR-0040 admits a new fact only when the deal evidence and complete
    // sampled history window were accepted at the same history-domain revision.
    if (observation.source_revision == 0 || observation.graph_instance_id == 0 ||
        observation.history_coverage_revision != observation.source_revision ||
        observation.history_window_from_msc > observation.history_window_to_msc ||
        observation.deal_time_msc < observation.history_window_from_msc ||
        observation.deal_time_msc > observation.history_window_to_msc)
        return ManagedOpeningExecutionValidationStatus::incomplete_history;
    if ((observation.known_fields & kManagedOpeningRequiredDealFields) !=
            kManagedOpeningRequiredDealFields ||
        observation.deal_ticket == 0 || observation.order_ticket == 0 ||
        observation.position_identifier == 0 ||
        !valid_broker_position_direction(observation.direction) ||
        observation.entry != BrokerDealEntry::in)
        return ManagedOpeningExecutionValidationStatus::invalid_deal;
    if (!observation.quantity_domain.valid())
        return ManagedOpeningExecutionValidationStatus::invalid_quantity;
    return ManagedOpeningExecutionValidationStatus::valid;
}

/// \struct ManagedOpeningAdmissionReceipt
/// \brief Preserves provenance of the first locally admitted observation.
struct ManagedOpeningAdmissionReceipt {
    std::uint64_t source_revision = 0; ///< Original deal evidence revision.
    std::uint64_t graph_instance_id = 0; ///< Original process-local graph identity.
    std::uint64_t history_coverage_revision = 0; ///< Original complete-window revision.
    std::int64_t history_window_from_msc = 0; ///< Inclusive coverage start.
    std::int64_t history_window_to_msc = 0; ///< Inclusive coverage end.

    /// \brief Tests the retained observation provenance.
    /// \return True when one complete sampled revision admitted the deal.
    bool valid() const {
        return source_revision != 0 && graph_instance_id != 0 &&
               history_coverage_revision == source_revision &&
               history_window_from_msc <= history_window_to_msc;
    }

    /// \brief Compares exact original admission provenance.
    /// \param other Receipt to compare.
    /// \return True when every retained source field matches.
    bool operator==(const ManagedOpeningAdmissionReceipt &other) const {
        return source_revision == other.source_revision &&
               graph_instance_id == other.graph_instance_id &&
               history_coverage_revision == other.history_coverage_revision &&
               history_window_from_msc == other.history_window_from_msc &&
               history_window_to_msc == other.history_window_to_msc;
    }
};

namespace detail {

inline void append_u64(std::vector<std::uint8_t> *bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes->push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void append_text(std::vector<std::uint8_t> *bytes, const std::string &value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes->insert(bytes->end(), value.begin(), value.end());
}

inline bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                     std::uint64_t *value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t))
        return false;
    *value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        *value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

inline bool read_text(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                      std::string *value,
                      std::size_t max_bytes = kManagedOpeningMaxCanonicalBytes) {
    std::uint64_t size = 0;
    if (!read_u64(bytes, offset, &size) || size > max_bytes ||
        size > bytes.size() - offset)
        return false;
    value->assign(reinterpret_cast<const char *>(bytes.data() + offset),
                  static_cast<std::size_t>(size));
    offset += static_cast<std::size_t>(size);
    return true;
}

inline void append_operation_key(std::vector<std::uint8_t> *bytes,
                                 const OperationKey &key) {
    append_text(bytes, key.account.server);
    append_u64(bytes, key.account.login);
    append_u64(bytes, key.trade_id);
    append_u64(bytes, key.operation_id);
}

inline void append_quantity(std::vector<std::uint8_t> *bytes,
                            const ManagedOpeningQuantity &quantity) {
    append_u64(bytes, quantity.canonical_scale());
    append_u64(bytes, quantity.broker_step_units());
    append_u64(bytes, quantity.broker_units());
    append_u64(bytes, quantity.integer_step_count());
    append_u64(bytes, quantity.managed_logical_units());
}

inline std::uint64_t managed_units_per_step(
    const ManagedOpeningQuantity &quantity) {
    if (!quantity.valid() || quantity.integer_step_count() == 0 ||
        quantity.managed_logical_units() % quantity.integer_step_count() != 0)
        return 0;
    return quantity.managed_logical_units() / quantity.integer_step_count();
}

inline bool same_quantity(const ManagedOpeningQuantity &left,
                          const ManagedOpeningQuantity &right) {
    return left.canonical_scale() == right.canonical_scale() &&
           left.broker_step_units() == right.broker_step_units() &&
           left.broker_units() == right.broker_units() &&
           left.integer_step_count() == right.integer_step_count() &&
           left.managed_logical_units() == right.managed_logical_units();
}

inline bool same_quantity_domain(const ManagedOpeningQuantity &left,
                                 const ManagedOpeningQuantity &right) {
    return left.canonical_scale() == right.canonical_scale() &&
           left.broker_step_units() == right.broker_step_units() &&
           managed_units_per_step(left) != 0 &&
           managed_units_per_step(left) == managed_units_per_step(right);
}

} // namespace detail

/// \class ManagedOpeningDealFact
/// \brief Immutable locally validated payload plus its first admission receipt.
///
/// `valid()` proves only internal consistency. It does not prove that the
/// journal record, broker history, or coverage existed. A future producer must
/// reload and compare those durable sources before using this fact as authority.
class ManagedOpeningDealFact {
public:
    /// \brief Canonical fact format version.
    static constexpr std::uint32_t k_schema_version = 1;

    /// \brief Builds a locally validated fact from candidate data.
    /// \param request Expected OPEN join constraints.
    /// \param observation Candidate observation, not an authority source.
    /// \return Immutable fact, or empty when local validation fails.
    static std::optional<ManagedOpeningDealFact> from_observation(
        const ManagedOpeningExecutionRequest &request,
        const ManagedOpeningDealObservation &observation) {
        if (validate_managed_opening_observation(request, observation) !=
            ManagedOpeningExecutionValidationStatus::valid)
            return std::nullopt;
        const auto quantity = derive_managed_opening_quantity(
            observation.observed_volume, observation.observed_step,
            observation.quantity_domain);
        if (!quantity)
            return std::nullopt;
        ManagedOpeningDealFact result(
            observation.operation_key, observation.operation_revision,
            observation.order_ticket, observation.deal_ticket,
            observation.position_identifier, observation.direction,
            observation.entry, observation.deal_time_msc, *quantity,
            {observation.source_revision, observation.graph_instance_id,
             observation.history_coverage_revision,
             observation.history_window_from_msc,
             observation.history_window_to_msc});
        return result.valid() ? std::optional<ManagedOpeningDealFact>(std::move(result))
                              : std::nullopt;
    }

    /// \brief Decodes one exact canonical fact representation.
    /// \param bytes Versioned bytes returned by canonical_bytes().
    /// \return Locally valid fact, or empty for any malformed/non-canonical input.
    static std::optional<ManagedOpeningDealFact> from_canonical_bytes(
        const std::vector<std::uint8_t> &bytes) {
        if (bytes.size() > kManagedOpeningMaxCanonicalBytes)
            return std::nullopt;
        std::size_t offset = 0;
        std::string tag;
        std::uint64_t schema = 0;
        if (!detail::read_text(bytes, offset, &tag) ||
            tag != "mt5bridge-managed-opening-deal-fact" ||
            !detail::read_u64(bytes, offset, &schema) || schema != k_schema_version)
            return std::nullopt;

        OperationKey operation_key;
        std::uint64_t operation_revision = 0;
        std::uint64_t order_ticket = 0;
        std::uint64_t deal_ticket = 0;
        std::uint64_t position_identifier = 0;
        std::uint64_t direction_value = 0;
        std::uint64_t entry_value = 0;
        std::uint64_t deal_time_value = 0;
        std::uint64_t canonical_scale = 0;
        std::uint64_t broker_step_units = 0;
        std::uint64_t broker_units = 0;
        std::uint64_t integer_step_count = 0;
        std::uint64_t managed_logical_units = 0;
        std::uint64_t history_window_from_value = 0;
        std::uint64_t history_window_to_value = 0;
        ManagedOpeningAdmissionReceipt receipt;
        if (!detail::read_text(bytes, offset, &operation_key.account.server) ||
            !detail::read_u64(bytes, offset, &operation_key.account.login) ||
            !detail::read_u64(bytes, offset, &operation_key.trade_id) ||
            !detail::read_u64(bytes, offset, &operation_key.operation_id) ||
            !detail::read_u64(bytes, offset, &operation_revision) ||
            !detail::read_u64(bytes, offset, &order_ticket) ||
            !detail::read_u64(bytes, offset, &deal_ticket) ||
            !detail::read_u64(bytes, offset, &position_identifier) ||
            !detail::read_u64(bytes, offset, &direction_value) ||
            !detail::read_u64(bytes, offset, &entry_value) ||
            !detail::read_u64(bytes, offset, &deal_time_value) ||
            !detail::read_u64(bytes, offset, &canonical_scale) ||
            !detail::read_u64(bytes, offset, &broker_step_units) ||
            !detail::read_u64(bytes, offset, &broker_units) ||
            !detail::read_u64(bytes, offset, &integer_step_count) ||
            !detail::read_u64(bytes, offset, &managed_logical_units) ||
            !detail::read_u64(bytes, offset, &receipt.source_revision) ||
            !detail::read_u64(bytes, offset, &receipt.graph_instance_id) ||
            !detail::read_u64(bytes, offset, &receipt.history_coverage_revision) ||
            !detail::read_u64(bytes, offset, &history_window_from_value) ||
            !detail::read_u64(bytes, offset, &history_window_to_value) ||
            offset != bytes.size() || canonical_scale > (std::numeric_limits<std::uint32_t>::max)())
            return std::nullopt;
        receipt.history_window_from_msc =
            static_cast<std::int64_t>(history_window_from_value);
        receipt.history_window_to_msc =
            static_cast<std::int64_t>(history_window_to_value);
        if (direction_value > static_cast<std::uint64_t>(BrokerPositionDirection::sell) ||
            entry_value > static_cast<std::uint64_t>(BrokerDealEntry::out_by))
            return std::nullopt;
        const auto quantity = ManagedOpeningQuantity::from_canonical_fields(
            static_cast<std::uint32_t>(canonical_scale), broker_step_units,
            broker_units, integer_step_count, managed_logical_units);
        if (!quantity)
            return std::nullopt;
        ManagedOpeningDealFact result(
            std::move(operation_key), operation_revision, order_ticket, deal_ticket,
            position_identifier, static_cast<BrokerPositionDirection>(direction_value),
            static_cast<BrokerDealEntry>(entry_value),
            static_cast<std::int64_t>(deal_time_value), *quantity, receipt);
        return result.valid() && result.canonical_bytes() == bytes
                   ? std::optional<ManagedOpeningDealFact>(std::move(result))
                   : std::nullopt;
    }

    /// \brief Returns the operation identity.
    /// \return Account-scoped OPEN key.
    const OperationKey &operation_key() const { return operation_key_; }
    /// \brief Returns the immutable broker-result source revision.
    /// \return Non-zero `OperationRecord::result_source_revision`.
    std::uint64_t operation_revision() const { return operation_revision_; }
    /// \brief Returns the OPEN order identity.
    /// \return Non-zero DEAL_ORDER ticket.
    std::uint64_t order_ticket() const { return order_ticket_; }
    /// \brief Returns the stable broker execution identity.
    /// \return Non-zero DEAL_TICKET.
    std::uint64_t deal_ticket() const { return deal_ticket_; }
    /// \brief Returns the broker position identity.
    /// \return Non-zero POSITION_IDENTIFIER.
    std::uint64_t position_identifier() const { return position_identifier_; }
    /// \brief Returns the deal direction.
    /// \return Buy or sell.
    BrokerPositionDirection direction() const { return direction_; }
    /// \brief Returns the immutable broker entry classification.
    /// \return The admitted OPEN entry value.
    BrokerDealEntry entry() const { return entry_; }
    /// \brief Returns the broker execution timestamp.
    /// \return DEAL_TIME in milliseconds.
    std::int64_t deal_time_msc() const { return deal_time_msc_; }
    /// \brief Returns the exact normalized deal quantity.
    /// \return B2 quantity-domain result.
    const ManagedOpeningQuantity &quantity() const { return quantity_; }
    /// \brief Returns the first admission provenance.
    /// \return Immutable receipt retained across later polling and restart.
    const ManagedOpeningAdmissionReceipt &admission_receipt() const { return receipt_; }

    /// \brief Tests local payload and receipt consistency only.
    /// \return True when all stored fields are self-consistent.
    bool valid() const {
        return operation_key_.valid() && operation_revision_ != 0 &&
               order_ticket_ != 0 && deal_ticket_ != 0 && position_identifier_ != 0 &&
               valid_broker_position_direction(direction_) &&
               entry_ == BrokerDealEntry::in && quantity_.valid() &&
               detail::managed_units_per_step(quantity_) != 0 && receipt_.valid() &&
               deal_time_msc_ >= receipt_.history_window_from_msc &&
               deal_time_msc_ <= receipt_.history_window_to_msc &&
               canonical_bytes().size() <= kManagedOpeningMaxCanonicalBytes;
    }

    /// \brief Compares stable identity and immutable execution payload.
    /// \param other Newly observed locally valid fact.
    /// \return True when broker and journal facts match, ignoring later polling provenance.
    bool same_execution_payload(const ManagedOpeningDealFact &other) const {
        return operation_key_ == other.operation_key_ &&
               operation_revision_ == other.operation_revision_ &&
               order_ticket_ == other.order_ticket_ &&
               deal_ticket_ == other.deal_ticket_ &&
               position_identifier_ == other.position_identifier_ &&
               direction_ == other.direction_ && entry_ == other.entry_ &&
               deal_time_msc_ == other.deal_time_msc_ &&
               detail::same_quantity(quantity_, other.quantity_);
    }

    /// \brief Returns the canonical durable bytes, including original provenance.
    /// \return Versioned little-endian fact representation.
    std::vector<std::uint8_t> canonical_bytes() const {
        std::vector<std::uint8_t> bytes;
        detail::append_text(&bytes, "mt5bridge-managed-opening-deal-fact");
        detail::append_u64(&bytes, k_schema_version);
        detail::append_operation_key(&bytes, operation_key_);
        detail::append_u64(&bytes, operation_revision_);
        detail::append_u64(&bytes, order_ticket_);
        detail::append_u64(&bytes, deal_ticket_);
        detail::append_u64(&bytes, position_identifier_);
        detail::append_u64(&bytes, static_cast<std::uint64_t>(direction_));
        detail::append_u64(&bytes, static_cast<std::uint64_t>(entry_));
        detail::append_u64(&bytes, static_cast<std::uint64_t>(deal_time_msc_));
        detail::append_quantity(&bytes, quantity_);
        detail::append_u64(&bytes, receipt_.source_revision);
        detail::append_u64(&bytes, receipt_.graph_instance_id);
        detail::append_u64(&bytes, receipt_.history_coverage_revision);
        detail::append_u64(&bytes,
                           static_cast<std::uint64_t>(receipt_.history_window_from_msc));
        detail::append_u64(&bytes,
                           static_cast<std::uint64_t>(receipt_.history_window_to_msc));
        return bytes;
    }

    /// \brief Compares exact durable fact content, including the first receipt.
    /// \param other Fact to compare.
    /// \return True when canonical durable content matches.
    bool operator==(const ManagedOpeningDealFact &other) const {
        return same_execution_payload(other) && receipt_ == other.receipt_;
    }

private:
    ManagedOpeningDealFact(OperationKey operation_key,
                           std::uint64_t operation_revision,
                           std::uint64_t order_ticket,
                           std::uint64_t deal_ticket,
                           std::uint64_t position_identifier,
                           BrokerPositionDirection direction,
                           BrokerDealEntry entry,
                           std::int64_t deal_time_msc,
                           ManagedOpeningQuantity quantity,
                           ManagedOpeningAdmissionReceipt receipt)
        : operation_key_(std::move(operation_key)),
          operation_revision_(operation_revision), order_ticket_(order_ticket),
          deal_ticket_(deal_ticket), position_identifier_(position_identifier),
          direction_(direction), entry_(entry), deal_time_msc_(deal_time_msc),
          quantity_(std::move(quantity)), receipt_(receipt) {}

    OperationKey operation_key_;
    std::uint64_t operation_revision_ = 0;
    std::uint64_t order_ticket_ = 0;
    std::uint64_t deal_ticket_ = 0;
    std::uint64_t position_identifier_ = 0;
    BrokerPositionDirection direction_ = BrokerPositionDirection::unknown;
    BrokerDealEntry entry_ = BrokerDealEntry::unknown;
    std::int64_t deal_time_msc_ = 0;
    ManagedOpeningQuantity quantity_;
    ManagedOpeningAdmissionReceipt receipt_;
};

/// \enum ManagedOpeningExecutionReplayStatus
/// \brief Classifies a fresh observation against one committed deal fact.
enum class ManagedOpeningExecutionReplayStatus : std::uint8_t {
    new_execution = 0,
    already_committed = 1,
    conflict = 2,
};

/// \brief Classifies replay by stable account-scoped deal identity and payload.
/// \param committed Original immutable fact; its receipt is never replaced.
/// \param observed Newly observed local fact.
/// \return Idempotent replay, conflict, or a distinct execution.
inline ManagedOpeningExecutionReplayStatus classify_managed_opening_replay(
    const ManagedOpeningDealFact &committed,
    const ManagedOpeningDealFact &observed) {
    if (committed.operation_key().account != observed.operation_key().account ||
        committed.deal_ticket() != observed.deal_ticket())
        return ManagedOpeningExecutionReplayStatus::new_execution;
    return committed.same_execution_payload(observed)
               ? ManagedOpeningExecutionReplayStatus::already_committed
               : ManagedOpeningExecutionReplayStatus::conflict;
}

/// \class ManagedOpeningExecutionFrontier
/// \brief Canonical cumulative executions for one OPEN and broker position.
///
/// This type deliberately has no requested volume or terminal flag. Execution
/// quantity and lifecycle terminality are independent facts under ADR-0040.
class ManagedOpeningExecutionFrontier {
public:
    /// \brief Builds a canonical frontier from compatible deal facts.
    /// \param entries Locally valid facts for one OPEN identity.
    /// \return Frontier, or empty when identity, domain, order, or sums conflict.
    static std::optional<ManagedOpeningExecutionFrontier> create(
        std::vector<ManagedOpeningDealFact> entries) {
        if (entries.empty())
            return std::nullopt;
        std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
            return left.deal_ticket() < right.deal_ticket();
        });
        const auto &basis = entries.front();
        if (!basis.valid())
            return std::nullopt;
        std::uint64_t broker_units = 0;
        std::uint64_t managed_units = 0;
        for (std::size_t index = 0; index != entries.size(); ++index) {
            const auto &entry = entries[index];
            if (!entry.valid() ||
                (index != 0 && entry.deal_ticket() <= entries[index - 1].deal_ticket()) ||
                !compatible_identity(basis, entry) ||
                broker_units > (std::numeric_limits<std::uint64_t>::max)() -
                                   entry.quantity().broker_units() ||
                managed_units > (std::numeric_limits<std::uint64_t>::max)() -
                                    entry.quantity().managed_logical_units())
                return std::nullopt;
            broker_units += entry.quantity().broker_units();
            managed_units += entry.quantity().managed_logical_units();
        }
        return ManagedOpeningExecutionFrontier(
            std::move(entries), broker_units, managed_units);
    }

    /// \brief Returns canonically deal-ticket-sorted facts.
    /// \return Immutable cumulative execution set.
    const std::vector<ManagedOpeningDealFact> &entries() const { return entries_; }
    /// \brief Returns cumulative normalized broker units.
    /// \return Overflow-checked sum across entries.
    std::uint64_t cumulative_broker_units() const { return cumulative_broker_units_; }
    /// \brief Returns cumulative managed logical units.
    /// \return Overflow-checked sum across entries.
    std::uint64_t cumulative_managed_logical_units() const {
        return cumulative_managed_logical_units_;
    }

    /// \brief Returns the canonical versioned frontier bytes.
    /// \return Length-delimited canonical facts and cumulative sums.
    std::vector<std::uint8_t> canonical_bytes() const {
        std::vector<std::uint8_t> bytes;
        detail::append_text(&bytes, "mt5bridge-managed-opening-frontier");
        detail::append_u64(&bytes, 1);
        detail::append_u64(&bytes, cumulative_broker_units_);
        detail::append_u64(&bytes, cumulative_managed_logical_units_);
        detail::append_u64(&bytes, static_cast<std::uint64_t>(entries_.size()));
        for (const auto &entry : entries_) {
            const auto fact = entry.canonical_bytes();
            detail::append_u64(&bytes, static_cast<std::uint64_t>(fact.size()));
            bytes.insert(bytes.end(), fact.begin(), fact.end());
        }
        return bytes;
    }

    /// \brief Compares exact canonical frontier content.
    /// \param other Frontier to compare.
    /// \return True when every fact and sum match.
    bool operator==(const ManagedOpeningExecutionFrontier &other) const {
        return cumulative_broker_units_ == other.cumulative_broker_units_ &&
               cumulative_managed_logical_units_ ==
                   other.cumulative_managed_logical_units_ &&
               entries_ == other.entries_;
    }

    /// \brief Tests whether one fact belongs to this frontier identity/domain.
    /// \param candidate Fact to compare with the canonical first entry.
    /// \return True for the same operation, order, position, direction, and domain.
    bool accepts_identity(const ManagedOpeningDealFact &candidate) const {
        return !entries_.empty() && compatible_identity(entries_.front(), candidate);
    }

private:
    static bool compatible_identity(const ManagedOpeningDealFact &basis,
                                    const ManagedOpeningDealFact &candidate) {
        return basis.operation_key() == candidate.operation_key() &&
               basis.operation_revision() == candidate.operation_revision() &&
               basis.order_ticket() == candidate.order_ticket() &&
               basis.position_identifier() == candidate.position_identifier() &&
               basis.direction() == candidate.direction() &&
               detail::same_quantity_domain(basis.quantity(), candidate.quantity());
    }

    ManagedOpeningExecutionFrontier(std::vector<ManagedOpeningDealFact> entries,
                                    std::uint64_t broker_units,
                                    std::uint64_t managed_units)
        : entries_(std::move(entries)), cumulative_broker_units_(broker_units),
          cumulative_managed_logical_units_(managed_units) {}

    std::vector<ManagedOpeningDealFact> entries_;
    std::uint64_t cumulative_broker_units_ = 0;
    std::uint64_t cumulative_managed_logical_units_ = 0;
};

/// \enum ManagedOpeningExecutionFrontierAppendStatus
/// \brief Reports one immutable cumulative-frontier extension.
enum class ManagedOpeningExecutionFrontierAppendStatus : std::uint8_t {
    appended = 0,
    duplicate = 1,
    conflict = 2,
    incompatible_identity = 3,
};

/// \struct ManagedOpeningExecutionFrontierAppendResult
/// \brief Carries a newly built frontier or a fail-closed status.
struct ManagedOpeningExecutionFrontierAppendResult {
    ManagedOpeningExecutionFrontierAppendStatus status =
        ManagedOpeningExecutionFrontierAppendStatus::conflict;
    std::optional<ManagedOpeningExecutionFrontier> frontier;

    /// \brief Tests whether extension produced a valid immutable frontier.
    /// \return True only for an appended result.
    bool appended() const {
        return status == ManagedOpeningExecutionFrontierAppendStatus::appended &&
               frontier.has_value();
    }
};

/// \brief Extends a cumulative frontier without rewriting committed facts.
///
/// Source revisions are retained as admission provenance, but are not ordered
/// here: ObservationGraph revisions are local to one graph instance. A future
/// authoritative producer must establish source freshness in its own durable
/// revision domain before admitting a fact.
/// \param frontier Existing immutable frontier.
/// \param fact New locally validated deal fact.
/// \return New frontier or duplicate/conflict/fail-closed status.
inline ManagedOpeningExecutionFrontierAppendResult append_managed_opening_execution(
    const ManagedOpeningExecutionFrontier &frontier,
    const ManagedOpeningDealFact &fact) {
    const auto found = std::lower_bound(
        frontier.entries().begin(), frontier.entries().end(), fact.deal_ticket(),
        [](const auto &entry, std::uint64_t ticket) {
            return entry.deal_ticket() < ticket;
        });
    if (found != frontier.entries().end() && found->deal_ticket() == fact.deal_ticket())
        return {found->same_execution_payload(fact)
                    ? ManagedOpeningExecutionFrontierAppendStatus::duplicate
                    : ManagedOpeningExecutionFrontierAppendStatus::conflict,
                std::nullopt};
    if (!frontier.accepts_identity(fact))
        return {ManagedOpeningExecutionFrontierAppendStatus::incompatible_identity,
                std::nullopt};
    auto entries = frontier.entries();
    entries.push_back(fact);
    auto extended = ManagedOpeningExecutionFrontier::create(std::move(entries));
    if (!extended)
        return {ManagedOpeningExecutionFrontierAppendStatus::conflict, std::nullopt};
    return {ManagedOpeningExecutionFrontierAppendStatus::appended,
            std::move(extended)};
}

} // namespace mt5bridge::dispatch

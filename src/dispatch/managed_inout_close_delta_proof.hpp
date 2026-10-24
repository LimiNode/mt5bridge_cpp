#pragma once

/// \file dispatch/managed_inout_close_delta_proof.hpp
/// \brief Defines the private authoritative proof for one managed INOUT close.

#include "managed_trade.hpp"

#include <mt5bridge/dispatch/broker_reversal.hpp>
#include <mt5bridge/dispatch/broker_reversal_reconciliation.hpp>
#include <mt5bridge/dispatch/journal_types.hpp>
#include <mt5bridge/dispatch/managed_deal_settlement.hpp>
#include <mt5bridge/dispatch/managed_ownership_basis.hpp>
#include <mt5bridge/reconciliation/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace mt5bridge::dispatch {

/// \struct ManagedExposureFrontier
/// \brief Compact owner-authoritative exposure boundary used by an INOUT proof.
struct ManagedExposureFrontier {
    AccountKey account; ///< Account scope of the managed trade.
    TradeId trade_id = 0; ///< Managed logical trade identity.
    OperationId operation_id = 0; ///< Close operation whose exposure is changing.
    managed_trade::Volume open_volume = 0; ///< Confirmed open exposure.
    managed_trade::Volume pending_remainder_volume = 0; ///< Unfilled entry remainder.
    managed_trade::Volume slice_result_volume = 0; ///< Cumulative result of this slice.
    std::uint64_t observation_epoch = 0; ///< Owner observation epoch at the boundary.
    managed_trade::OperationState slice_state = managed_trade::OperationState::idle;
    bool close_requested = false; ///< Close obligation was durably requested.
    bool close_satisfied = false; ///< Close obligation was satisfied at this boundary.

    /// \brief Tests the local shape of one exposure boundary.
    /// \return True only for a valid close-operation snapshot.
    bool valid() const {
        return account.valid() && trade_id != 0 && operation_id != 0 &&
               slice_state != managed_trade::OperationState::idle &&
               close_requested;
    }

    /// \brief Compares all captured exposure fields.
    /// \param other Boundary to compare.
    /// \return True when every field matches.
    bool operator==(const ManagedExposureFrontier &other) const {
        return account == other.account && trade_id == other.trade_id &&
               operation_id == other.operation_id && open_volume == other.open_volume &&
               pending_remainder_volume == other.pending_remainder_volume &&
               slice_result_volume == other.slice_result_volume &&
               observation_epoch == other.observation_epoch &&
               slice_state == other.slice_state && close_requested == other.close_requested &&
               close_satisfied == other.close_satisfied;
    }
};

/// \class ManagedInoutCloseDeltaProof
/// \brief Sealed proof that one authoritative INOUT deal reduced managed exposure.
class ManagedInoutCloseDeltaProof {
public:
    /// \brief Returns the managed operation receiving the close delta.
    /// \return Operation identity.
    const OperationKey &operation_key() const { return operation_key_; }

    /// \brief Returns the broker deal bound to the close intent.
    /// \return DEAL_TICKET.
    std::uint64_t deal_ticket() const { return deal_ticket_; }

    /// \brief Returns the ownership-basis identity bound by the proof.
    /// \return Durable broker/operation composite key.
    const ManagedOwnershipBasisKey &ownership_key() const { return ownership_key_; }

    /// \brief Returns the logical close intent volume.
    /// \return Requested managed close volume.
    managed_trade::Volume managed_close_intent_volume() const {
        return managed_close_intent_volume_;
    }

    /// \brief Returns the positive managed contribution derived from exposure.
    /// \return Exact logical units reduced by this deal.
    managed_trade::Volume managed_logical_delta() const { return managed_logical_delta_; }

    /// \brief Returns the pre-close managed exposure boundary.
    /// \return Owner-authoritative pre-state summary.
    const ManagedExposureFrontier &pre_exposure() const { return pre_exposure_; }

    /// \brief Returns the post-close managed exposure boundary.
    /// \return Owner-authoritative post-state summary.
    const ManagedExposureFrontier &post_exposure() const { return post_exposure_; }

    /// \brief Returns the broker provenance copied into the proof binding.
    /// \return Durable reversal provenance.
    const BrokerReversalProvenance &broker_provenance() const {
        return broker_provenance_;
    }

    /// \brief Returns fresh graph provenance for the managed delta.
    /// \return Observation provenance.
    const ManagedSettlementProvenance &observation_provenance() const {
        return observation_provenance_;
    }

    /// \brief Returns the immutable proof digest.
    /// \return Non-zero digest covering all proof fields.
    std::uint64_t evidence_digest() const { return evidence_digest_; }

    /// \brief Tests the sealed proof's local invariants.
    /// \return True only for a positive, identity-consistent delta proof.
    bool valid() const {
        if (!operation_key_.valid() || deal_ticket_ == 0 ||
            !ownership_key_.valid() || ownership_key_.operation_key != operation_key_ ||
            ownership_key_.broker_key.account != operation_key_.account ||
            ownership_key_.broker_key.deal_ticket != deal_ticket_ ||
            !broker_provenance_.valid() || managed_close_intent_volume_ == 0 ||
            managed_logical_delta_ == 0 || !pre_exposure_.valid() ||
            !post_exposure_.valid() || pre_exposure_.account != operation_key_.account ||
            post_exposure_.account != operation_key_.account ||
            pre_exposure_.trade_id != operation_key_.trade_id ||
            post_exposure_.trade_id != operation_key_.trade_id ||
            pre_exposure_.operation_id != operation_key_.operation_id ||
            post_exposure_.operation_id != operation_key_.operation_id ||
            pre_exposure_.pending_remainder_volume !=
                post_exposure_.pending_remainder_volume ||
            pre_exposure_.open_volume <= post_exposure_.open_volume ||
            pre_exposure_.open_volume - post_exposure_.open_volume !=
                managed_logical_delta_ ||
            post_exposure_.slice_result_volume < pre_exposure_.slice_result_volume ||
            post_exposure_.slice_result_volume - pre_exposure_.slice_result_volume !=
                managed_logical_delta_ ||
            post_exposure_.slice_result_volume > managed_close_intent_volume_ ||
            managed_logical_delta_ > managed_close_intent_volume_ ||
            pre_exposure_.close_satisfied || !observation_provenance_.valid() ||
            evidence_digest_ == 0)
            return false;
        return true;
    }

    /// \brief Compares two sealed proofs for exact replay.
    /// \param other Proof to compare.
    /// \return True when every durable proof field matches.
    bool operator==(const ManagedInoutCloseDeltaProof &other) const {
        return operation_key_ == other.operation_key_ &&
               deal_ticket_ == other.deal_ticket_ && ownership_key_ == other.ownership_key_ &&
               managed_close_intent_volume_ == other.managed_close_intent_volume_ &&
               managed_logical_delta_ == other.managed_logical_delta_ &&
               pre_exposure_ == other.pre_exposure_ &&
               post_exposure_ == other.post_exposure_ &&
               broker_provenance_ == other.broker_provenance_ &&
               observation_provenance_ == other.observation_provenance_ &&
               evidence_digest_ == other.evidence_digest_;
    }

private:
    ManagedInoutCloseDeltaProof(
        OperationKey operation_key, std::uint64_t deal_ticket,
        ManagedOwnershipBasisKey ownership_key, managed_trade::Volume intent_volume,
        managed_trade::Volume logical_delta, ManagedExposureFrontier pre_exposure,
        ManagedExposureFrontier post_exposure, BrokerReversalProvenance broker_provenance,
        ManagedSettlementProvenance observation_provenance, std::uint64_t evidence_digest)
        : operation_key_(std::move(operation_key)),
          deal_ticket_(deal_ticket),
          ownership_key_(std::move(ownership_key)),
          managed_close_intent_volume_(intent_volume),
          managed_logical_delta_(logical_delta),
          pre_exposure_(std::move(pre_exposure)),
          post_exposure_(std::move(post_exposure)),
          broker_provenance_(broker_provenance),
          observation_provenance_(observation_provenance),
          evidence_digest_(evidence_digest) {}

    OperationKey operation_key_;
    std::uint64_t deal_ticket_ = 0;
    ManagedOwnershipBasisKey ownership_key_;
    managed_trade::Volume managed_close_intent_volume_ = 0;
    managed_trade::Volume managed_logical_delta_ = 0;
    ManagedExposureFrontier pre_exposure_;
    ManagedExposureFrontier post_exposure_;
    BrokerReversalProvenance broker_provenance_;
    ManagedSettlementProvenance observation_provenance_;
    std::uint64_t evidence_digest_ = 0;

    friend class ManagedInoutCloseDeltaProofProducer;
};

namespace detail {

inline std::optional<std::string> inout_proof_snapshot_text(const char *value,
                                                            std::size_t capacity) {
    const auto *terminator = static_cast<const char *>(std::memchr(value, '\0', capacity));
    if (!terminator)
        return std::nullopt;
    return std::string(value, static_cast<std::size_t>(terminator - value));
}

inline std::optional<ReconciliationPredicate> inout_proof_history_predicate(
    const OperationRecord &operation, std::uint64_t deal_ticket) {
    if (!operation.reconciliation_descriptor || deal_ticket == 0)
        return std::nullopt;
    std::optional<ReconciliationPredicate> result;
    for (const auto &predicate : operation.reconciliation_descriptor->predicates) {
        if (predicate.kind != ReconciliationPredicateKind::history_deal_present)
            continue;
        if (result.has_value())
            return std::nullopt;
        result = predicate;
    }
    if (!result || !result->history_window || !result->history_window->valid() ||
        !mt5bridge::detail::operation_binds_deal(operation, deal_ticket))
        return std::nullopt;
    return result;
}

inline std::optional<Mt5DealSnapshot> inout_proof_deal(const ObservationGraph &graph,
                                                       std::uint64_t deal_ticket) {
    std::optional<Mt5DealSnapshot> result;
    for (const auto &deal : graph.history_deals()) {
        if (deal.ticket != deal_ticket)
            continue;
        if (result.has_value())
            return std::nullopt;
        result = deal;
    }
    return result;
}

inline ManagedExposureFrontier inout_proof_frontier(
    const managed_trade::ManagedTradeState &state) {
    return {AccountKey{}, state.trade_id.value, state.slice.operation_id,
            state.open_volume, state.pending_remainder_volume, state.slice.result_volume,
            state.observation_epoch, state.slice.state, state.close_obligation.requested,
            state.close_obligation.satisfied};
}

class InoutProofDigest {
public:
    void add_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            value_ ^= static_cast<std::uint8_t>(value >> shift);
            value_ *= 1099511628211ULL;
        }
    }

    void add_text(const std::string &value) {
        add_u64(static_cast<std::uint64_t>(value.size()));
        for (const unsigned char byte : value) {
            value_ ^= byte;
            value_ *= 1099511628211ULL;
        }
    }

    void add_frontier(const ManagedExposureFrontier &frontier) {
        add_text(frontier.account.server);
        add_u64(frontier.account.login);
        add_u64(frontier.trade_id);
        add_u64(frontier.operation_id);
        add_u64(frontier.open_volume);
        add_u64(frontier.pending_remainder_volume);
        add_u64(frontier.slice_result_volume);
        add_u64(frontier.observation_epoch);
        add_u64(static_cast<std::uint64_t>(frontier.slice_state));
        add_u64(frontier.close_requested ? 1 : 0);
        add_u64(frontier.close_satisfied ? 1 : 0);
    }

    void add_broker_provenance(const BrokerReversalProvenance &provenance) {
        add_u64(provenance.graph_instance_id);
        add_u64(provenance.pre_graph_revision);
        add_u64(provenance.post_graph_revision);
        add_u64(provenance.pre_positions_revision);
        add_u64(provenance.post_positions_revision);
        add_u64(provenance.history_deals_revision);
        add_u64(provenance.evidence_digest);
    }

    std::uint64_t value() const { return value_ == 0 ? 1 : value_; }

private:
    std::uint64_t value_ = 1469598103934665603ULL;
};

inline std::uint64_t inout_proof_digest(const OperationRecord &operation,
                                        const ManagedOwnershipBasis &basis,
                                        const BrokerReversalRecord &reversal,
                                        const ManagedExposureFrontier &pre_exposure,
                                        const ManagedExposureFrontier &post_exposure,
                                        const ManagedSettlementProvenance &observation,
                                        managed_trade::Volume intent_volume,
                                        managed_trade::Volume logical_delta) {
    InoutProofDigest digest;
    digest.add_text("mt5bridge-managed-inout-close-proof-v1");
    digest.add_text(operation.key.account.server);
    digest.add_u64(operation.key.account.login);
    digest.add_u64(operation.key.trade_id);
    digest.add_u64(operation.key.operation_id);
    digest.add_u64(operation.revision);
    digest.add_u64(basis.operation_revision);
    digest.add_u64(reversal.deal_ticket);
    digest.add_u64(intent_volume);
    digest.add_u64(logical_delta);
    digest.add_frontier(pre_exposure);
    digest.add_frontier(post_exposure);
    digest.add_broker_provenance(reversal.provenance);
    digest.add_u64(observation.graph_instance_id);
    digest.add_u64(observation.graph_revision);
    digest.add_u64(observation.history_deals_revision);
    digest.add_u64(observation.evidence_digest);
    return digest.value();
}

} // namespace detail

/// \class ManagedInoutCloseDeltaProofProducer
/// \brief Privately derives a sealed managed delta from fresh INOUT evidence.
class ManagedInoutCloseDeltaProofProducer {
public:
    /// \brief Builds a proof without accepting caller-supplied logical units.
    /// \param operation Durable allocation-gated close operation.
    /// \param basis Durable ownership association for the same operation/deal.
    /// \param reversal Durable broker-only INOUT decomposition.
    /// \param baseline Fresh-cycle baseline bound to the operation descriptor.
    /// \param graph Account-scoped graph containing complete post-deal evidence.
    /// \param pre_state Owner state immediately before applying this close delta.
    /// \param post_state Owner candidate after applying this close delta.
    /// \return Sealed proof, or empty when any source is stale, ambiguous, or mismatched.
    static std::optional<ManagedInoutCloseDeltaProof> create(
        const OperationRecord &operation, const ManagedOwnershipBasis &basis,
        const BrokerReversalRecord &reversal, const ReconciliationBaseline &baseline,
        const ObservationGraph &graph, const managed_trade::ManagedTradeState &pre_state,
        const managed_trade::ManagedTradeState &post_state) {
        if (!operation.valid() || operation.operation_kind != OperationKind::close ||
            operation.settlement_mode != ManagedSettlementMode::allocation_gated ||
            !journal_at_least_result_persisted(operation.journal_state) ||
            (operation.operation_state != OperationState::reconciling &&
             operation.operation_state != OperationState::partially_filled &&
             operation.operation_state != OperationState::filled) ||
            !operation.reconciliation_descriptor ||
            operation.reconciliation_descriptor->requested_volume == 0 ||
            operation.settled_volume == 0 ||
            operation.settled_volume >
                operation.reconciliation_descriptor->requested_volume ||
            !basis.valid() || !reversal.valid() || !baseline.valid() || !graph.bound() ||
            operation.key.account != graph.account_key() ||
            baseline.account() != operation.key.account ||
            operation.reconciliation_descriptor->account != operation.key.account ||
            operation.reconciliation_descriptor->trade_id != operation.key.trade_id ||
            operation.reconciliation_descriptor->operation_id != operation.key.operation_id ||
            operation.reconciliation_descriptor->baseline.graph_instance_id() !=
                baseline.graph_instance_id() ||
            operation.reconciliation_descriptor->baseline.graph_revision() !=
                baseline.graph_revision() ||
            basis.account != operation.key.account ||
            basis.basis_key.operation_key != operation.key ||
            !(basis.basis_key.broker_key == reversal.key()) ||
            basis.operation_revision != operation.revision ||
            !(basis.broker_provenance == reversal.provenance) ||
            reversal.account != graph.account_key())
            return std::nullopt;

        const auto predicate =
            detail::inout_proof_history_predicate(operation, reversal.deal_ticket);
        if (!predicate || !graph.history_deals_covered(*predicate->history_window,
                                                        baseline.history_deals_revision()))
            return std::nullopt;
        const auto history_revision = graph.domain_revision(ObservationDomain::history_deals);
        if (history_revision == 0 || history_revision <= baseline.history_deals_revision() ||
            graph.history_deal_evidence_revision(reversal.deal_ticket) != history_revision ||
            reversal.provenance.graph_instance_id != graph.instance_id() ||
            reversal.provenance.post_graph_revision <= baseline.graph_revision() ||
            reversal.provenance.post_graph_revision > graph.revision() ||
            reversal.provenance.history_deals_revision != history_revision)
            return std::nullopt;

        constexpr std::uint64_t kRequiredDealFields =
            MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
            MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
            MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME |
            MT5BRIDGE_DEAL_KNOWN_SYMBOL;
        const auto deal = detail::inout_proof_deal(graph, reversal.deal_ticket);
        const auto symbol = deal ? detail::inout_proof_snapshot_text(
                                        deal->symbol, sizeof(deal->symbol))
                                 : std::nullopt;
        if (!deal || !symbol || *symbol != reversal.symbol ||
            (deal->known_fields & kRequiredDealFields) != kRequiredDealFields ||
            deal->ticket != reversal.deal_ticket || deal->order_ticket != reversal.order_ticket ||
            deal->position_id != reversal.deal_position_id ||
            deal->entry != static_cast<std::uint32_t>(BrokerDealEntry::inout) ||
            deal->time_msc < predicate->history_window->from_msc ||
            deal->time_msc > predicate->history_window->to_msc)
            return std::nullopt;
        const auto normalized_deal = normalize_broker_volume(
            deal->volume,
            BrokerVolumeNormalization{reversal.deal_volume.scale,
                                      reversal.deal_volume.step_units});
        if (!normalized_deal || !(*normalized_deal == reversal.deal_volume))
            return std::nullopt;

        std::size_t matching_fresh_deals = 0;
        for (const auto &candidate : graph.history_deals()) {
            if (candidate.order_ticket != reversal.order_ticket ||
                candidate.position_id != reversal.deal_position_id ||
                candidate.time_msc < predicate->history_window->from_msc ||
                candidate.time_msc > predicate->history_window->to_msc ||
                graph.history_deal_evidence_revision(candidate.ticket) != history_revision)
                continue;
            if (++matching_fresh_deals != 1 || candidate.ticket != reversal.deal_ticket)
                return std::nullopt;
        }

        const bool valid_pre_slice_state =
            pre_state.slice.state == managed_trade::OperationState::reconciling ||
            pre_state.slice.state == managed_trade::OperationState::partially_filled ||
            pre_state.slice.state == managed_trade::OperationState::submitting;
        const bool valid_post_slice_state =
            post_state.slice.state == managed_trade::OperationState::partially_filled ||
            post_state.slice.state == managed_trade::OperationState::filled;
        if (!pre_state.valid() || !post_state.valid() ||
            pre_state.trade_id.value != operation.key.trade_id ||
            post_state.trade_id.value != operation.key.trade_id ||
            pre_state.slice.operation_id != operation.key.operation_id ||
            post_state.slice.operation_id != operation.key.operation_id ||
            pre_state.slice.kind != managed_trade::OperationKind::close ||
            post_state.slice.kind != managed_trade::OperationKind::close ||
            pre_state.slice.requested_volume !=
                operation.reconciliation_descriptor->requested_volume ||
            post_state.slice.requested_volume != pre_state.slice.requested_volume ||
            !valid_pre_slice_state || !valid_post_slice_state ||
            pre_state.pending_remainder_volume != post_state.pending_remainder_volume ||
            pre_state.open_volume <= post_state.open_volume ||
            pre_state.open_volume - post_state.open_volume == 0 ||
            post_state.slice.result_volume < pre_state.slice.result_volume ||
            post_state.slice.result_volume - pre_state.slice.result_volume !=
                pre_state.open_volume - post_state.open_volume ||
            post_state.slice.result_volume >
                operation.reconciliation_descriptor->requested_volume ||
            operation.settled_volume != post_state.slice.result_volume ||
            pre_state.plan.state != post_state.plan.state ||
            pre_state.plan.target_volume != post_state.plan.target_volume ||
            pre_state.plan.max_slice_volume != post_state.plan.max_slice_volume ||
            pre_state.plan.max_operations != post_state.plan.max_operations ||
            pre_state.plan.slice_count != post_state.plan.slice_count ||
            post_state.observation_epoch < pre_state.observation_epoch ||
            pre_state.close_obligation.requested != post_state.close_obligation.requested ||
            pre_state.close_obligation.ever_requested !=
                post_state.close_obligation.ever_requested ||
            pre_state.close_obligation.satisfied || !pre_state.close_obligation.requested)
            return std::nullopt;

        auto pre_exposure = detail::inout_proof_frontier(pre_state);
        auto post_exposure = detail::inout_proof_frontier(post_state);
        pre_exposure.account = operation.key.account;
        post_exposure.account = operation.key.account;
        const auto logical_delta = pre_state.open_volume - post_state.open_volume;
        const auto intent_volume =
            operation.reconciliation_descriptor->requested_volume;
        const ManagedSettlementProvenance observation{
            graph.instance_id(), graph.revision(), history_revision,
            reversal.provenance.evidence_digest};
        if (!observation.valid())
            return std::nullopt;
        const auto digest = detail::inout_proof_digest(
            operation, basis, reversal, pre_exposure, post_exposure, observation,
            intent_volume, logical_delta);
        ManagedInoutCloseDeltaProof result(
            operation.key, reversal.deal_ticket, basis.basis_key, intent_volume,
            logical_delta, pre_exposure, post_exposure, reversal.provenance, observation,
            digest);
        return result.valid() ? std::optional<ManagedInoutCloseDeltaProof>(std::move(result))
                              : std::nullopt;
    }
};

} // namespace mt5bridge::dispatch

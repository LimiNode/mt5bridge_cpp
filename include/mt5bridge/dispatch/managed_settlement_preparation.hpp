#pragma once

/// \file dispatch/managed_settlement_preparation.hpp
/// \brief Defines the durable staged settlement and ownership preparation seam.

#include "managed_deal_settlement.hpp"
#include "managed_ownership_basis.hpp"

#include <algorithm>
#include <cstdint>

namespace mt5bridge {

namespace managed_settlement_preparation_detail {

inline bool same_window(const std::optional<ObservationWindow> &left,
                        const std::optional<ObservationWindow> &right) {
    return left.has_value() == right.has_value() &&
           (!left || (left->from_msc == right->from_msc &&
                      left->to_msc == right->to_msc));
}

inline bool same_descriptor(const std::optional<ReconciliationDescriptor> &left,
                            const std::optional<ReconciliationDescriptor> &right) {
    if (left.has_value() != right.has_value())
        return false;
    if (!left)
        return true;
    if (left->account != right->account || left->trade_id != right->trade_id ||
        left->operation_id != right->operation_id ||
        left->requested_volume != right->requested_volume ||
        left->settled_state != right->settled_state ||
        left->baseline.account() != right->baseline.account() ||
        left->baseline.graph_instance_id() != right->baseline.graph_instance_id() ||
        left->baseline.graph_revision() != right->baseline.graph_revision() ||
        left->baseline.active_orders_revision() != right->baseline.active_orders_revision() ||
        left->baseline.positions_revision() != right->baseline.positions_revision() ||
        left->baseline.history_orders_revision() !=
            right->baseline.history_orders_revision() ||
        left->baseline.history_deals_revision() != right->baseline.history_deals_revision() ||
        left->predicates.size() != right->predicates.size())
        return false;
    for (std::size_t index = 0; index != left->predicates.size(); ++index) {
        const auto &a = left->predicates[index];
        const auto &b = right->predicates[index];
        if (a.kind != b.kind || a.ticket != b.ticket ||
            a.baseline_present != b.baseline_present ||
            a.correlation_id != b.correlation_id ||
            a.expected_transition != b.expected_transition ||
            !same_window(a.history_window, b.history_window))
            return false;
    }
    return true;
}

inline bool same_operation(const OperationRecord &left, const OperationRecord &right) {
    return left.key == right.key && left.operation_kind == right.operation_kind &&
           left.settlement_mode == right.settlement_mode &&
           left.request_payload == right.request_payload &&
           left.result_payload == right.result_payload &&
           left.operation_state == right.operation_state &&
           left.journal_state == right.journal_state && left.revision == right.revision &&
           left.fencing_token == right.fencing_token &&
           left.settled_volume == right.settled_volume &&
           same_descriptor(left.reconciliation_descriptor, right.reconciliation_descriptor) &&
           left.reconciliation_bindings == right.reconciliation_bindings;
}

} // namespace managed_settlement_preparation_detail

/// \struct ManagedStagedOwnershipAssociation
/// \brief Sealed ownership identity retained until the target journal is settled.
struct ManagedStagedOwnershipAssociation {
    AccountKey account; ///< Account shared by all staged source identities.
    ManagedOwnershipBasisKey basis_key; ///< Broker reversal and operation identity.
    BrokerReversalProvenance broker_provenance; ///< Durable broker evidence identity.
    std::uint64_t base_operation_revision = 0; ///< Revision observed before staging.
    std::uint64_t target_operation_revision = 0; ///< Revision to be settled.
    std::uint64_t close_intent_digest = 0; ///< Digest of the bound close intent.

    /// \brief Tests identity and staged revision invariants.
    /// \return True only for a complete association without a volume claim.
    bool valid() const {
        return account.valid() && basis_key.valid() && broker_provenance.valid() &&
               account == basis_key.broker_key.account &&
               account == basis_key.operation_key.account && base_operation_revision != 0 &&
               target_operation_revision == base_operation_revision + 1 &&
               close_intent_digest != 0;
    }

    /// \brief Converts the staged association into the ordinary settled basis.
    /// \return Basis pinned to the target operation revision.
    ManagedOwnershipBasis settled_basis() const {
        return {account, basis_key, broker_provenance, target_operation_revision};
    }

    /// \brief Compares the exact staged association payload.
    /// \param other Association to compare.
    /// \return True when every staged field matches.
    bool operator==(const ManagedStagedOwnershipAssociation &other) const {
        return account == other.account && basis_key == other.basis_key &&
               broker_provenance == other.broker_provenance &&
               base_operation_revision == other.base_operation_revision &&
               target_operation_revision == other.target_operation_revision &&
               close_intent_digest == other.close_intent_digest;
    }
};

/// \struct ManagedSettlementPreparation
/// \brief Exact durable recipe for one allocation-gated close publication.
struct ManagedSettlementPreparation {
    ManagedDealSettlement fact; ///< Immutable per-deal managed contribution.
    ManagedSettlementFrontier frontier; ///< Complete cumulative settlement frontier.
    OperationRecord target_operation; ///< Exact operation candidate for journal CAS.
    std::uint64_t base_operation_revision = 0; ///< Expected current journal revision.
    ManagedStagedOwnershipAssociation staged_ownership; ///< Pending ownership identity.

    /// \brief Tests cross-record preparation invariants.
    /// \return True only for a complete, target-bound staged recipe.
    bool valid() const {
        if (!fact.valid() || !frontier.valid() || !target_operation.valid() ||
            !staged_ownership.valid() || !(fact.key.account == frontier.account) ||
            fact.key.operation_key != frontier.operation_key ||
            fact.source_operation_revision > frontier.operation_revision ||
            target_operation.key != frontier.operation_key ||
            target_operation.revision != frontier.operation_revision ||
            target_operation.operation_kind != OperationKind::close ||
            (target_operation.operation_state != OperationState::partially_filled &&
             target_operation.operation_state != OperationState::filled) ||
            !target_operation.reconciliation_descriptor ||
            target_operation.reconciliation_descriptor->requested_volume == 0 ||
            target_operation.settled_volume != frontier.settled_volume ||
            base_operation_revision == 0 ||
            target_operation.revision != base_operation_revision + 1 ||
            staged_ownership.account != target_operation.key.account ||
            staged_ownership.basis_key.operation_key != target_operation.key ||
            staged_ownership.basis_key.broker_key.account != target_operation.key.account ||
            staged_ownership.basis_key.broker_key.deal_ticket != fact.key.deal_ticket ||
            staged_ownership.target_operation_revision != target_operation.revision)
            return false;
        const auto fact_entry = std::find_if(
            frontier.entries.begin(), frontier.entries.end(),
            [this](const auto &entry) {
                return entry.deal_ticket == fact.key.deal_ticket &&
                       entry.source_operation_revision == fact.source_operation_revision &&
                       entry.managed_logical_units == fact.managed_logical_units;
            });
        return fact_entry != frontier.entries.end() &&
               detail::operation_binds_deal(target_operation, fact.key.deal_ticket) &&
               managed_settlement_frontier_digest(frontier) != 0;
    }

    /// \brief Compares the exact durable preparation recipe.
    /// \param other Preparation to compare.
    /// \return True when all fields match.
    bool operator==(const ManagedSettlementPreparation &other) const {
        return fact == other.fact && frontier == other.frontier &&
               managed_settlement_preparation_detail::same_operation(
                   target_operation, other.target_operation) &&
               base_operation_revision == other.base_operation_revision &&
               staged_ownership == other.staged_ownership;
    }
};

/// \enum ManagedSettlementPreparationStatus
/// \brief Reports durable preparation decisions.
enum class ManagedSettlementPreparationStatus {
    prepared,
    already_prepared,
    conflict,
    invalid_record,
    missing_operation_record,
    invalid_operation_record,
    io_error,
};

/// \enum ManagedSettlementFinalizeStatus
/// \brief Reports staged publication and recovery decisions.
enum class ManagedSettlementFinalizeStatus {
    finalized,
    already_finalized,
    pending_allocation,
    lease_not_held,
    conflict,
    invalid_record,
    missing_operation_record,
    invalid_operation_record,
    missing_source,
    invalid_source,
    io_error,
};

/// \enum ManagedSettlementPreparationLoadStatus
/// \brief Reports lookup state for one durable preparation manifest.
enum class ManagedSettlementPreparationLoadStatus {
    found,
    not_found,
    invalid_record,
    io_error,
};

/// \struct ManagedSettlementPreparationLoadResult
/// \brief Carries one exact durable preparation lookup.
struct ManagedSettlementPreparationLoadResult {
    ManagedSettlementPreparationLoadStatus status =
        ManagedSettlementPreparationLoadStatus::io_error;
    std::optional<ManagedSettlementPreparation> record;

    /// \brief Tests whether an exact preparation was loaded.
    /// \return True only when status is `found` with a recipe.
    bool found() const {
        return status == ManagedSettlementPreparationLoadStatus::found && record.has_value();
    }
};

/// \class DurableManagedSettlementPreparationStore
/// \brief Lookup seam for exact durable staged settlement manifests.
class DurableManagedSettlementPreparationStore {
public:
    virtual ~DurableManagedSettlementPreparationStore() = default;

    /// \brief Loads the exact manifest for one operation frontier and deal.
    /// \param operation_key Operation identity addressed by the manifest.
    /// \param operation_revision Exact prepared target revision.
    /// \param deal_ticket Deal identity addressed by the manifest.
    /// \return Status-bearing durable preparation result.
    virtual ManagedSettlementPreparationLoadResult load(
        const OperationKey &operation_key, std::uint64_t operation_revision,
        std::uint64_t deal_ticket) const = 0;
};

} // namespace mt5bridge

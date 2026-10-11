#pragma once

/// \file dispatch/managed_ownership_basis.hpp
/// \brief Defines the durable association between a close operation and a reversal.

#include "broker_allocation_envelope.hpp"
#include "journal_store.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge {

/// \struct ManagedOwnershipBasisKey
/// \brief Composite identity allowing one reversal to have many future owners.
struct ManagedOwnershipBasisKey {
    BrokerReversalKey broker_key; ///< Broker reversal being considered.
    OperationKey operation_key; ///< Managed operation receiving the basis.

    /// \brief Tests whether both sides of the composite identity are usable.
    /// \return True only for valid broker and managed-operation keys.
    bool valid() const { return broker_key.valid() && operation_key.valid(); }

    /// \brief Compares two composite identities.
    /// \param other Identity to compare.
    /// \return True when both identities match exactly.
    bool operator==(const ManagedOwnershipBasisKey &other) const {
        return broker_key == other.broker_key && operation_key == other.operation_key;
    }

    /// \brief Orders identities for deterministic durable scans.
    /// \param other Identity to compare.
    /// \return True when this identity sorts before `other`.
    bool operator<(const ManagedOwnershipBasisKey &other) const {
        if (!(broker_key == other.broker_key))
            return broker_key < other.broker_key;
        return operation_key < other.operation_key;
    }
};

/// \struct ManagedOwnershipBasis
/// \brief Durable association linking one settled close operation to one reversal.
///
/// This slice records only durable identity and source revisions. It does not
/// attribute any per-deal volume: the operation's cumulative `settled_volume`
/// cannot be used as a cap for one broker deal without a separate logical
/// settlement proof.
struct ManagedOwnershipBasis {
    AccountKey account; ///< Account shared by both durable source records.
    ManagedOwnershipBasisKey basis_key; ///< Broker/operation composite identity.
    BrokerReversalProvenance broker_provenance; ///< Exact broker envelope provenance.
    std::uint64_t operation_revision = 0; ///< Pinned durable operation revision.

    /// \brief Returns the composite durable identity.
    /// \return Broker reversal and managed operation key.
    const ManagedOwnershipBasisKey &key() const { return basis_key; }

    /// \brief Tests all local identity and association invariants.
    /// \return True only for a non-empty, account-consistent basis.
    bool valid() const {
        return account.valid() && basis_key.valid() &&
               account == basis_key.broker_key.account &&
               account == basis_key.operation_key.account && broker_provenance.valid() &&
               operation_revision != 0;
    }

    /// \brief Compares every durable basis field for idempotent replay.
    /// \param other Basis to compare.
    /// \return True when all serialized fields match.
    bool operator==(const ManagedOwnershipBasis &other) const {
        return account == other.account && basis_key == other.basis_key &&
               broker_provenance == other.broker_provenance &&
               operation_revision == other.operation_revision;
    }
};

/// \enum ManagedOwnershipBasisCommitStatus
/// \brief Reports the result of a proof-gated ownership-basis commit.
enum class ManagedOwnershipBasisCommitStatus {
    committed,                 ///< A new basis was durably written.
    already_committed,         ///< The exact basis already exists.
    conflict,                  ///< The composite identity has another basis.
    invalid_record,            ///< The candidate failed local validation.
    missing_operation_record,  ///< The referenced operation is not durable.
    invalid_operation_record,  ///< The operation is not an eligible close proof.
    missing_allocation_envelope, ///< The broker envelope is not durable.
    invalid_allocation_envelope, ///< The envelope or provenance does not match.
    io_error,                  ///< A source or destination store failed.
};

/// \enum ManagedOwnershipBasisLoadStatus
/// \brief Reports a composite-key ownership-basis lookup.
enum class ManagedOwnershipBasisLoadStatus {
    found,          ///< A complete valid basis was loaded.
    not_found,      ///< No basis exists for the composite key.
    invalid_record, ///< Storage exists but is malformed or inconsistent.
    io_error,       ///< Storage could not complete the read.
};

/// \struct ManagedOwnershipBasisLoadResult
/// \brief Carries a status-bearing ownership-basis load result.
struct ManagedOwnershipBasisLoadResult {
    ManagedOwnershipBasisLoadStatus status = ManagedOwnershipBasisLoadStatus::io_error;
    std::optional<ManagedOwnershipBasis> record;

    /// \brief Tests whether a complete basis was loaded.
    /// \return True only when status is `found` and a record is present.
    bool found() const {
        return status == ManagedOwnershipBasisLoadStatus::found && record.has_value();
    }
};

/// \enum ManagedOwnershipBasisScanStatus
/// \brief Reports an all-or-nothing ownership-basis scan.
enum class ManagedOwnershipBasisScanStatus {
    complete,       ///< Every basis was loaded and validated.
    invalid_record, ///< At least one basis is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \struct ManagedOwnershipBasisScanResult
/// \brief Carries all bases discovered during restart recovery.
struct ManagedOwnershipBasisScanResult {
    ManagedOwnershipBasisScanStatus status = ManagedOwnershipBasisScanStatus::io_error;
    std::vector<ManagedOwnershipBasis> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when no record was malformed or unreadable.
    bool complete() const { return status == ManagedOwnershipBasisScanStatus::complete; }
};

/// \enum ManagedOwnershipBasisProofStatus
/// \brief Reports whether durable operation and broker evidence authorize a basis.
enum class ManagedOwnershipBasisProofStatus {
    valid,                       ///< Both durable source records match the basis.
    invalid_record,              ///< The candidate failed local validation.
    missing_operation_record,    ///< No operation record exists for the key.
    invalid_operation_record,    ///< Operation state/evidence cannot authorize a basis.
    missing_allocation_envelope, ///< No broker-only envelope exists for the reversal.
    invalid_allocation_envelope, ///< Envelope identity or provenance mismatches.
    io_error,                    ///< A source store could not complete its read.
};

/// \class DurableManagedOwnershipBasisStore
/// \brief Persistence seam for durable operation/reversal associations.
class DurableManagedOwnershipBasisStore {
public:
    virtual ~DurableManagedOwnershipBasisStore() = default;

    /// \brief Commits a basis only after both durable source proofs are checked.
    /// \param record Candidate ownership basis.
    /// \param journal_store Durable source of the managed operation.
    /// \param allocation_store Durable source of the broker envelope.
    /// \return Commit, replay, conflict, validation, or I/O status.
    virtual ManagedOwnershipBasisCommitStatus commit(
        const ManagedOwnershipBasis &record,
        const DurableJournalStore &journal_store,
        const DurableBrokerAllocationStore &allocation_store) = 0;

    /// \brief Loads one basis by its broker/operation composite identity.
    /// \param key Composite broker and managed-operation identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    virtual ManagedOwnershipBasisLoadResult load(
        const ManagedOwnershipBasisKey &key) const = 0;

    /// \brief Enumerates every basis for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    virtual ManagedOwnershipBasisScanResult scan() const = 0;
};

namespace detail {

inline bool operation_binds_deal(const OperationRecord &operation,
                                 std::uint64_t deal_ticket) {
    if (!operation.reconciliation_descriptor || deal_ticket == 0)
        return false;

    std::size_t history_deal_predicates = 0;
    std::uint64_t resolved_ticket = 0;
    for (const auto &predicate : operation.reconciliation_descriptor->predicates) {
        if (predicate.kind != ReconciliationPredicateKind::history_deal_present)
            continue;
        if (++history_deal_predicates != 1)
            return false;
        resolved_ticket = predicate.ticket;
        if (resolved_ticket == 0) {
            if (predicate.correlation_id == 0)
                return false;
            std::size_t binding_matches = 0;
            for (const auto &binding : operation.reconciliation_bindings) {
                if (binding.correlation_id == predicate.correlation_id) {
                    if (++binding_matches != 1 || !binding.valid())
                        return false;
                    resolved_ticket = binding.broker_ticket;
                }
            }
            if (binding_matches != 1)
                return false;
        }
    }
    return history_deal_predicates == 1 && resolved_ticket == deal_ticket;
}

} // namespace detail

/// \brief Validates a basis against its durable operation and broker envelope.
/// \param basis Candidate operation-scoped ownership basis.
/// \param journal_store Durable source of managed operation state.
/// \param allocation_store Durable source of broker-only envelope state.
/// \return Proof-validation status.
inline ManagedOwnershipBasisProofStatus validate_managed_ownership_basis(
    const ManagedOwnershipBasis &basis,
    const DurableJournalStore &journal_store,
    const DurableBrokerAllocationStore &allocation_store) {
    if (!basis.valid())
        return ManagedOwnershipBasisProofStatus::invalid_record;

    const auto operation = journal_store.load(basis.basis_key.operation_key);
    if (operation.status == StoreLoadStatus::not_found)
        return ManagedOwnershipBasisProofStatus::missing_operation_record;
    if (operation.status == StoreLoadStatus::invalid_record)
        return ManagedOwnershipBasisProofStatus::invalid_operation_record;
    if (operation.status == StoreLoadStatus::io_error)
        return ManagedOwnershipBasisProofStatus::io_error;
    if (!operation.found() || !operation.record || !operation.record->valid())
        return ManagedOwnershipBasisProofStatus::invalid_operation_record;

    const auto &source = *operation.record;
    if (source.key != basis.basis_key.operation_key || source.key.account != basis.account ||
        source.revision != basis.operation_revision || source.operation_kind != OperationKind::close ||
        (source.operation_state != OperationState::partially_filled &&
         source.operation_state != OperationState::filled) ||
        !journal_at_least_result_persisted(source.journal_state) ||
        !source.reconciliation_descriptor ||
        (source.reconciliation_descriptor->settled_state !=
             OperationState::partially_filled &&
         source.reconciliation_descriptor->settled_state != OperationState::filled) ||
        source.reconciliation_descriptor->requested_volume == 0 ||
        source.settled_volume == 0 ||
        source.settled_volume > source.reconciliation_descriptor->requested_volume ||
        !detail::operation_binds_deal(source, basis.basis_key.broker_key.deal_ticket))
        return ManagedOwnershipBasisProofStatus::invalid_operation_record;

    const auto envelope = allocation_store.load(basis.basis_key.broker_key);
    if (envelope.status == BrokerAllocationLoadStatus::not_found)
        return ManagedOwnershipBasisProofStatus::missing_allocation_envelope;
    if (envelope.status == BrokerAllocationLoadStatus::invalid_record)
        return ManagedOwnershipBasisProofStatus::invalid_allocation_envelope;
    if (envelope.status == BrokerAllocationLoadStatus::io_error)
        return ManagedOwnershipBasisProofStatus::io_error;
    if (!envelope.found() || !envelope.record || !envelope.record->valid() ||
        envelope.record->account != basis.account ||
        !(envelope.record->key() == basis.basis_key.broker_key) ||
        !(envelope.record->broker_provenance == basis.broker_provenance))
        return ManagedOwnershipBasisProofStatus::invalid_allocation_envelope;

    return ManagedOwnershipBasisProofStatus::valid;
}

} // namespace mt5bridge

#pragma once

/// \file dispatch/managed_allocation.hpp
/// \brief Defines the durable close-leg allocation entry and store contract.

#include "broker_allocation_envelope.hpp"
#include "lease.hpp"
#include "managed_broker_volume_admission.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge {

struct ManagedSettlementPreparation;
class DurableManagedSettlementPreparationStore;

/// \enum ManagedAllocationLeg
/// \brief Identifies the broker leg consumed by an allocation entry.
enum class ManagedAllocationLeg : std::uint32_t {
    close = 0, ///< Managed close allocation against the broker close leg.
};

/// \struct ManagedAllocationKey
/// \brief Composite identity for one immutable managed close allocation.
struct ManagedAllocationKey {
    BrokerReversalKey broker_key; ///< Broker reversal containing the consumed leg.
    ManagedDealSettlementKey managed_key; ///< Managed operation/deal source.
    ManagedAllocationLeg leg = ManagedAllocationLeg::close; ///< Consumed broker leg.

    /// \brief Tests account, deal, and supported-leg consistency.
    /// \return True only for a close allocation addressing the same deal.
    bool valid() const {
        return broker_key.valid() && managed_key.valid() &&
               broker_key.account == managed_key.account &&
               broker_key.deal_ticket == managed_key.deal_ticket &&
               leg == ManagedAllocationLeg::close;
    }

    /// \brief Compares exact allocation identity.
    /// \param other Key to compare.
    /// \return True when every identity component matches.
    bool operator==(const ManagedAllocationKey &other) const {
        return broker_key == other.broker_key && managed_key == other.managed_key &&
               leg == other.leg;
    }

    /// \brief Orders keys for deterministic scans.
    /// \param other Key to compare.
    /// \return True when this key sorts before `other`.
    bool operator<(const ManagedAllocationKey &other) const {
        if (!(broker_key == other.broker_key))
            return broker_key < other.broker_key;
        if (!(managed_key == other.managed_key))
            return managed_key < other.managed_key;
        return static_cast<std::uint32_t>(leg) <
               static_cast<std::uint32_t>(other.leg);
    }
};

/// \struct ManagedAllocationEntry
/// \brief Immutable proof that one managed fact consumes broker close volume.
struct ManagedAllocationEntry {
    AccountKey account; ///< Account shared by every source proof.
    ManagedAllocationKey allocation_key; ///< Broker/managed/leg identity.
    ManagedSettlementProvenance settlement_provenance; ///< Per-deal evidence.
    BrokerReversalProvenance broker_provenance; ///< Broker reversal evidence.
    std::uint64_t settlement_source_operation_revision = 0; ///< Settlement source revision.
    std::uint64_t settlement_frontier_revision = 0; ///< Exact consumed frontier revision.
    std::optional<std::uint64_t> settlement_frontier_digest; ///< Canonical frontier digest.
    std::uint64_t basis_operation_revision = 0; ///< Ownership-basis revision.
    std::uint64_t managed_logical_units = 0; ///< Exact managed quantity consumed.
    BrokerVolume broker_volume; ///< Exact step-normalized close quantity consumed.

    /// \brief Tests local identity, provenance, and incremental revision invariants.
    /// \return True only for a positive close allocation entry.
    bool valid() const {
        return account.valid() && allocation_key.valid() &&
               account == allocation_key.broker_key.account &&
               account == allocation_key.managed_key.account &&
               settlement_provenance.valid() && broker_provenance.valid() &&
               settlement_source_operation_revision != 0 &&
               basis_operation_revision != 0 &&
               settlement_source_operation_revision <= basis_operation_revision &&
               ((!settlement_frontier_digest && settlement_frontier_revision == 0) ||
                (settlement_frontier_digest && *settlement_frontier_digest != 0 &&
                 settlement_frontier_revision != 0)) &&
               managed_logical_units != 0 && broker_volume.valid();
    }

    /// \brief Compares every durable field for exact replay.
    /// \param other Entry to compare.
    /// \return True when all serialized fields match.
    bool operator==(const ManagedAllocationEntry &other) const {
        return account == other.account && allocation_key == other.allocation_key &&
               settlement_provenance == other.settlement_provenance &&
               broker_provenance == other.broker_provenance &&
               settlement_source_operation_revision ==
                   other.settlement_source_operation_revision &&
               settlement_frontier_revision == other.settlement_frontier_revision &&
               settlement_frontier_digest == other.settlement_frontier_digest &&
               basis_operation_revision == other.basis_operation_revision &&
               managed_logical_units == other.managed_logical_units &&
               broker_volume == other.broker_volume;
    }
};

/// \enum ManagedAllocationCommitStatus
/// \brief Reports a serialized allocation publication decision.
enum class ManagedAllocationCommitStatus {
    committed, ///< A new immutable entry was durably published.
    already_committed, ///< The exact allocation entry already exists.
    conflict, ///< The allocation key exists with a different payload.
    invalid_record, ///< The entry or mapping is malformed or inconsistent.
    missing_source, ///< A required durable source is absent.
    invalid_source, ///< A durable source is malformed or mismatched.
    capacity_exhausted, ///< The broker close leg cannot fit the new quantity.
    lease_not_held, ///< Authoritative account fencing is absent or changed.
    io_error, ///< Storage or a source read failed.
};

/// \enum ManagedAllocationLoadStatus
/// \brief Reports lookup state for one allocation entry.
enum class ManagedAllocationLoadStatus { found, not_found, invalid_record, io_error };

/// \struct ManagedAllocationLoadResult
/// \brief Carries one status-bearing allocation lookup.
struct ManagedAllocationLoadResult {
    ManagedAllocationLoadStatus status = ManagedAllocationLoadStatus::io_error;
    std::optional<ManagedAllocationEntry> record;

    /// \brief Tests whether a valid entry was found.
    /// \return True only when status is `found` and a record is present.
    bool found() const {
        return status == ManagedAllocationLoadStatus::found && record.has_value();
    }
};

/// \enum ManagedAllocationScanStatus
/// \brief Reports an all-or-nothing historical allocation scan.
enum class ManagedAllocationScanStatus { complete, invalid_record, io_error };

/// \struct ManagedAllocationScanResult
/// \brief Carries entries recovered with immutable-source validation.
struct ManagedAllocationScanResult {
    ManagedAllocationScanStatus status = ManagedAllocationScanStatus::io_error;
    std::vector<ManagedAllocationEntry> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when every entry and capacity sum is valid.
    bool complete() const { return status == ManagedAllocationScanStatus::complete; }
};

/// \class DurableManagedAllocationStore
/// \brief Persistence seam for serialized managed close-leg allocation entries.
class DurableManagedAllocationStore {
public:
    virtual ~DurableManagedAllocationStore() = default;

    /// \brief Commits or replays one entry under source admission and fencing.
    /// \param entry Candidate immutable allocation entry.
    /// \param mapping Canonical mapping returned by source admission.
    /// \param journal_store Mutable operation source used only for a new entry.
    /// \param settlement_store Durable per-deal settlement source.
    /// \param broker_store Durable broker reversal source.
    /// \param basis_store Durable operation/reversal association source.
    /// \param envelope_store Durable broker-only envelope source.
    /// \param lease Authoritative account-scoped writer ownership.
    /// \return Commit, replay, conflict, source, capacity, lease, or I/O status.
    virtual ManagedAllocationCommitStatus commit(
        const ManagedAllocationEntry &entry,
        const ManagedBrokerVolumeMapping &mapping,
        const DurableJournalStore &journal_store,
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const SingleWriterLease &lease) = 0;

    /// \brief Commits an allocation against a prepared, not-yet-committed settlement.
    /// \param entry Candidate immutable allocation entry with a frontier reference.
    /// \param mapping Canonical broker-volume mapping derived from the prepared fact.
    /// \param preparation Exact staged settlement recipe.
    /// \param journal_store Durable operation source used for base validation.
    /// \param settlement_store Durable historical settlement source for old entries.
    /// \param broker_store Durable broker reversal source.
    /// \param basis_store Durable ordinary ownership source for old entries.
    /// \param envelope_store Durable broker envelope source.
    /// \param lease Authoritative account-scoped writer ownership.
    /// \return Commit, replay, source, capacity, fencing, or I/O status.
    virtual ManagedAllocationCommitStatus commit_staged(
        const ManagedAllocationEntry &entry,
        const ManagedBrokerVolumeMapping &mapping,
        const ManagedSettlementPreparation &preparation,
        const DurableManagedSettlementPreparationStore &preparation_store,
        const DurableJournalStore &journal_store,
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const SingleWriterLease &lease) = 0;

    /// \brief Loads one structurally valid entry by exact key.
    /// \param key Allocation identity.
    /// \return Status-bearing local record result.
    virtual ManagedAllocationLoadResult load(const ManagedAllocationKey &key) const = 0;

    /// \brief Scans entries and validates immutable sources and capacity.
    /// \param settlement_store Durable settlement source.
    /// \param broker_store Durable broker reversal source.
    /// \param basis_store Durable ownership-basis source.
    /// \param envelope_store Durable broker envelope source.
    /// \return Complete historical entries or an all-or-nothing failure.
    virtual ManagedAllocationScanResult scan(
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store) const = 0;

    /// \brief Scans entries while admitting exact pending preparation manifests.
    /// \param settlement_store Durable committed settlement source.
    /// \param broker_store Durable broker reversal source.
    /// \param basis_store Durable ownership-basis source.
    /// \param envelope_store Durable broker envelope source.
    /// \param preparation_store Durable exact staged preparation source.
    /// \return Complete historical and prepared-backed entries, or failure.
    virtual ManagedAllocationScanResult scan_recovery(
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const DurableManagedSettlementPreparationStore &preparation_store) const = 0;
};

} // namespace mt5bridge

#pragma once

/// \file dispatch/managed_allocation.hpp
/// \brief Defines the durable link between one broker reversal and one managed operation.

#include "broker_reversal.hpp"

#include "journal_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace mt5bridge {

/// \struct ManagedAllocationVolume
/// \brief Step-normalized managed or unallocated leg, including an explicit zero.
///
/// `BrokerVolume` intentionally rejects zero because broker observations describe
/// positive quantities. Allocation records must also represent a fully allocated
/// leg, so this companion type admits zero while retaining the same exact scale
/// and step metadata.
struct ManagedAllocationVolume {
    std::uint64_t units = 0; ///< Allocated units; zero is an explicit empty leg.
    std::uint32_t scale = 0; ///< Decimal denominator exponent, at most nine.
    std::uint64_t step_units = 0; ///< Positive configured symbol-step units.

    /// \brief Tests whether the volume is exactly representable at its step.
    /// \return True when zero or a positive step-aligned quantity is encoded.
    bool valid() const {
        return scale <= 9 && step_units != 0 && units % step_units == 0;
    }

    /// \brief Compares exact allocation volume metadata and units.
    /// \param other Volume to compare.
    /// \return True when all serialized fields match.
    bool operator==(const ManagedAllocationVolume &other) const {
        return units == other.units && scale == other.scale &&
               step_units == other.step_units;
    }
};

/// \struct ManagedAllocationRecord
/// \brief Immutable, proof-linked managed allocation for one broker reversal.
///
/// This first allocation slice permits exactly one immutable managed link per
/// broker reversal. It does not select FIFO, LIFO, or pro-rata policy and does
/// not mutate managed exposure. Any broker leg not assigned to the linked
/// operation is retained explicitly in the corresponding unallocated field.
struct ManagedAllocationRecord {
    AccountKey account; ///< Account scope shared by all linked identities.
    BrokerReversalKey broker_key; ///< Durable broker reversal being allocated.
    OperationKey operation_key; ///< Managed TradeId/OperationId receiving the link.
    BrokerReversalProvenance broker_provenance; ///< Exact provenance of the source record.
    ManagedAllocationVolume managed_close_leg; ///< Portion assigned to the operation.
    ManagedAllocationVolume managed_reverse_open_leg; ///< Portion assigned to the operation.
    ManagedAllocationVolume unallocated_close_leg; ///< Broker close remainder.
    ManagedAllocationVolume unallocated_reverse_open_leg; ///< Broker open remainder.

    /// \brief Returns the immutable allocation identity.
    /// \return The single broker reversal key allocated by this record.
    const BrokerReversalKey &key() const { return broker_key; }

    /// \brief Tests identity, proof, volume, and conservation invariants.
    /// \return True only for a self-consistent allocation link.
    bool valid() const {
        if (!account.valid() || !broker_key.valid() || !operation_key.valid() ||
            account != broker_key.account || account != operation_key.account ||
            !broker_provenance.valid() ||
            !managed_close_leg.valid() || !managed_reverse_open_leg.valid() ||
            !unallocated_close_leg.valid() || !unallocated_reverse_open_leg.valid())
            return false;

        if (!same_shape(managed_close_leg, managed_reverse_open_leg) ||
            !same_shape(managed_close_leg, unallocated_close_leg) ||
            !same_shape(managed_close_leg, unallocated_reverse_open_leg))
            return false;

        if (managed_close_leg.units == 0 && managed_reverse_open_leg.units == 0)
            return false;

        return sums_without_overflow(managed_close_leg.units,
                                     unallocated_close_leg.units) &&
               sums_without_overflow(managed_reverse_open_leg.units,
                                     unallocated_reverse_open_leg.units);
    }

    /// \brief Compares all durable allocation fields for idempotent replay.
    /// \param other Record to compare.
    /// \return True when the records are byte-semantic equivalents.
    bool operator==(const ManagedAllocationRecord &other) const {
        return account == other.account && broker_key == other.broker_key &&
               operation_key == other.operation_key &&
               broker_provenance == other.broker_provenance &&
               managed_close_leg == other.managed_close_leg &&
               managed_reverse_open_leg == other.managed_reverse_open_leg &&
               unallocated_close_leg == other.unallocated_close_leg &&
               unallocated_reverse_open_leg == other.unallocated_reverse_open_leg;
    }

private:
    static bool same_shape(const ManagedAllocationVolume &left,
                           const ManagedAllocationVolume &right) {
        return left.scale == right.scale && left.step_units == right.step_units;
    }

    static bool sums_without_overflow(std::uint64_t left, std::uint64_t right) {
        return left <= (std::numeric_limits<std::uint64_t>::max)() - right;
    }
};

/// \enum ManagedAllocationCommitStatus
/// \brief Reports an immutable managed-allocation commit result.
enum class ManagedAllocationCommitStatus {
    committed,             ///< A new allocation was durably written.
    already_committed,     ///< The exact allocation already exists.
    conflict,              ///< The broker reversal already has another allocation.
    invalid_record,        ///< The candidate failed local validation.
    missing_broker_record, ///< No durable broker proof exists for the link.
    invalid_broker_record, ///< The linked broker proof is malformed or mismatched.
    io_error,              ///< Storage could not complete the operation.
};

/// \enum ManagedAllocationLoadStatus
/// \brief Reports a managed-allocation lookup result.
enum class ManagedAllocationLoadStatus {
    found,          ///< A complete valid allocation was loaded.
    not_found,      ///< No allocation exists for the requested broker key.
    invalid_record, ///< Storage exists but is malformed or inconsistent.
    io_error,       ///< Storage could not complete the read.
};

/// \struct ManagedAllocationLoadResult
/// \brief Carries a status-bearing managed-allocation load result.
struct ManagedAllocationLoadResult {
    ManagedAllocationLoadStatus status = ManagedAllocationLoadStatus::io_error;
    std::optional<ManagedAllocationRecord> record;

    /// \brief Tests whether a complete allocation was loaded.
    /// \return True only when status is `found` and a record is present.
    bool found() const {
        return status == ManagedAllocationLoadStatus::found && record.has_value();
    }
};

/// \enum ManagedAllocationScanStatus
/// \brief Reports an all-or-nothing allocation scan result.
enum class ManagedAllocationScanStatus {
    complete,       ///< Every allocation record was loaded and validated.
    invalid_record, ///< At least one record is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \enum ManagedAllocationProofStatus
/// \brief Reports whether a broker proof can authorize an allocation.
enum class ManagedAllocationProofStatus {
    valid,                 ///< The durable broker proof matches the allocation.
    invalid_record,        ///< The allocation failed local validation.
    missing_broker_record, ///< No broker record exists for the link.
    invalid_broker_record, ///< The broker record or its identity does not match.
    io_error,              ///< The broker store could not load the proof.
};

/// \struct ManagedAllocationScanResult
/// \brief Carries all durable managed-allocation links for restart recovery.
struct ManagedAllocationScanResult {
    ManagedAllocationScanStatus status = ManagedAllocationScanStatus::io_error;
    std::vector<ManagedAllocationRecord> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when the scan completed without corruption.
    bool complete() const { return status == ManagedAllocationScanStatus::complete; }
};

/// \class DurableManagedAllocationStore
/// \brief Persistence seam for immutable broker-to-managed allocation links.
class DurableManagedAllocationStore {
public:
    virtual ~DurableManagedAllocationStore() = default;

    /// \brief Commits one immutable allocation keyed by broker reversal.
    /// \param record Candidate allocation link.
    /// \param broker_store Durable broker-proof source that must authorize it.
    /// \return Commit, replay, conflict, validation, or I/O status.
    virtual ManagedAllocationCommitStatus commit(
        const ManagedAllocationRecord &record,
        const DurableBrokerReversalStore &broker_store) = 0;

    /// \brief Loads the allocation for one broker reversal.
    /// \param key Broker reversal identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    virtual ManagedAllocationLoadResult load(const BrokerReversalKey &key) const = 0;

    /// \brief Enumerates all allocation links for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    virtual ManagedAllocationScanResult scan() const = 0;
};

/// \brief Validates that a durable broker proof authorizes an allocation.
/// \param allocation Candidate managed allocation link.
/// \param broker_store Durable source of broker reversal proofs.
/// \return Proof-validation status.
inline ManagedAllocationProofStatus validate_managed_allocation_proof(
    const ManagedAllocationRecord &allocation,
    const DurableBrokerReversalStore &broker_store) {
    if (!allocation.valid())
        return ManagedAllocationProofStatus::invalid_record;

    const auto broker = broker_store.load(allocation.broker_key);
    if (broker.status == BrokerReversalLoadStatus::not_found)
        return ManagedAllocationProofStatus::missing_broker_record;
    if (broker.status == BrokerReversalLoadStatus::invalid_record)
        return ManagedAllocationProofStatus::invalid_broker_record;
    if (broker.status == BrokerReversalLoadStatus::io_error)
        return ManagedAllocationProofStatus::io_error;
    if (!broker.found() || !broker.record || !broker.record->valid() ||
        !(broker.record->key() == allocation.broker_key) ||
        broker.record->account != allocation.account ||
        !(broker.record->provenance == allocation.broker_provenance))
        return ManagedAllocationProofStatus::invalid_broker_record;

    const auto &source = *broker.record;
    const auto shape_matches = [&allocation](const BrokerVolume &broker_volume,
                                              const ManagedAllocationVolume &managed) {
        return broker_volume.scale == managed.scale &&
               broker_volume.step_units == managed.step_units;
    };
    if (!shape_matches(source.broker_close_leg, allocation.managed_close_leg) ||
        !shape_matches(source.broker_reverse_open_leg,
                       allocation.managed_reverse_open_leg) ||
        allocation.managed_close_leg.units > source.broker_close_leg.units ||
        allocation.managed_reverse_open_leg.units > source.broker_reverse_open_leg.units ||
        allocation.unallocated_close_leg.units !=
            source.broker_close_leg.units - allocation.managed_close_leg.units ||
        allocation.unallocated_reverse_open_leg.units !=
            source.broker_reverse_open_leg.units -
                allocation.managed_reverse_open_leg.units)
        return ManagedAllocationProofStatus::invalid_record;

    return ManagedAllocationProofStatus::valid;
}

/// \brief Commits an allocation only after its broker proof is durably present.
/// \param allocation Candidate managed allocation link.
/// \param broker_store Durable source of broker reversal proofs.
/// \param allocation_store Durable destination for allocation links.
/// \return Proof-gated commit status.
inline ManagedAllocationCommitStatus commit_managed_allocation(
    const ManagedAllocationRecord &allocation,
    const DurableBrokerReversalStore &broker_store,
    DurableManagedAllocationStore &allocation_store) {
    return allocation_store.commit(allocation, broker_store);
}

} // namespace mt5bridge

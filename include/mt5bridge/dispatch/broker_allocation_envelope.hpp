#pragma once

/// \file dispatch/broker_allocation_envelope.hpp
/// \brief Defines the durable broker-allocation envelope without ownership attribution.

#include "broker_reversal.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge {

/// \struct BrokerAllocationEnvelope
/// \brief Immutable envelope proving that one broker reversal is ready for later allocation.
///
/// This bounded slice deliberately contains no `TradeId`, `OperationKey`, or
/// managed leg. Both broker legs remain explicitly unallocated. A later
/// ownership ledger must add its own durable basis before assigning any volume
/// to a managed trade.
struct BrokerAllocationEnvelope {
    AccountKey account; ///< Account scope shared by the broker proof.
    BrokerReversalKey broker_key; ///< Durable broker reversal being reserved.
    BrokerReversalProvenance broker_provenance; ///< Exact source provenance.
    BrokerVolume broker_close_leg; ///< Broker-level close leg copied from the proof.
    BrokerVolume broker_reverse_open_leg; ///< Broker-level reverse-open leg copied from proof.
    BrokerVolume unallocated_close_leg; ///< Entire close leg remains unallocated.
    BrokerVolume unallocated_reverse_open_leg; ///< Entire reverse-open leg remains unallocated.

    /// \brief Returns the immutable envelope identity.
    /// \return The broker reversal key.
    const BrokerReversalKey &key() const { return broker_key; }

    /// \brief Tests identity, provenance, and complete-unallocated invariants.
    /// \return True only for a self-consistent broker envelope.
    bool valid() const {
        return account.valid() && broker_key.valid() &&
               account == broker_key.account && broker_provenance.valid() &&
               broker_close_leg.valid() && broker_reverse_open_leg.valid() &&
               unallocated_close_leg.valid() && unallocated_reverse_open_leg.valid() &&
               same_shape(broker_close_leg, broker_reverse_open_leg) &&
               same_shape(broker_close_leg, unallocated_close_leg) &&
               same_shape(broker_close_leg, unallocated_reverse_open_leg) &&
               unallocated_close_leg == broker_close_leg &&
               unallocated_reverse_open_leg == broker_reverse_open_leg;
    }

    /// \brief Compares all durable envelope fields for idempotent replay.
    /// \param other Envelope to compare.
    /// \return True when every serialized field matches.
    bool operator==(const BrokerAllocationEnvelope &other) const {
        return account == other.account && broker_key == other.broker_key &&
               broker_provenance == other.broker_provenance &&
               broker_close_leg == other.broker_close_leg &&
               broker_reverse_open_leg == other.broker_reverse_open_leg &&
               unallocated_close_leg == other.unallocated_close_leg &&
               unallocated_reverse_open_leg == other.unallocated_reverse_open_leg;
    }

private:
    static bool same_shape(const BrokerVolume &left, const BrokerVolume &right) {
        return left.scale == right.scale && left.step_units == right.step_units;
    }
};

/// \enum BrokerAllocationCommitStatus
/// \brief Reports an immutable broker-allocation envelope commit.
enum class BrokerAllocationCommitStatus {
    committed,             ///< A new envelope was durably written.
    already_committed,     ///< The exact envelope already exists.
    conflict,              ///< The broker key has another envelope.
    invalid_record,        ///< The candidate failed local validation.
    missing_broker_record, ///< No durable broker proof exists for the envelope.
    invalid_broker_record, ///< The linked broker proof is malformed or mismatched.
    io_error,              ///< Storage could not complete the operation.
};

/// \enum BrokerAllocationLoadStatus
/// \brief Reports a broker-allocation envelope lookup.
enum class BrokerAllocationLoadStatus {
    found,          ///< A complete valid envelope was loaded.
    not_found,      ///< No envelope exists for the requested broker key.
    invalid_record, ///< Storage exists but is malformed or inconsistent.
    io_error,       ///< Storage could not complete the read.
};

/// \struct BrokerAllocationLoadResult
/// \brief Carries a status-bearing envelope load result.
struct BrokerAllocationLoadResult {
    BrokerAllocationLoadStatus status = BrokerAllocationLoadStatus::io_error;
    std::optional<BrokerAllocationEnvelope> record;

    /// \brief Tests whether a complete envelope was loaded.
    /// \return True only when status is `found` and a record is present.
    bool found() const {
        return status == BrokerAllocationLoadStatus::found && record.has_value();
    }
};

/// \enum BrokerAllocationScanStatus
/// \brief Reports an all-or-nothing envelope scan.
enum class BrokerAllocationScanStatus {
    complete,       ///< Every envelope was loaded and validated.
    invalid_record, ///< At least one envelope is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \struct BrokerAllocationScanResult
/// \brief Carries all envelopes discovered during restart recovery.
struct BrokerAllocationScanResult {
    BrokerAllocationScanStatus status = BrokerAllocationScanStatus::io_error;
    std::vector<BrokerAllocationEnvelope> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when the scan completed without corruption.
    bool complete() const { return status == BrokerAllocationScanStatus::complete; }
};

/// \enum BrokerAllocationProofStatus
/// \brief Reports whether a durable broker proof authorizes an envelope.
enum class BrokerAllocationProofStatus {
    valid,                 ///< The durable broker proof matches the envelope.
    invalid_record,        ///< The envelope failed local validation.
    missing_broker_record, ///< No broker record exists for the envelope.
    invalid_broker_record, ///< The broker record or identity does not match.
    io_error,              ///< The broker store could not load the proof.
};

/// \class DurableBrokerAllocationStore
/// \brief Persistence seam for broker-only allocation envelopes.
class DurableBrokerAllocationStore {
public:
    virtual ~DurableBrokerAllocationStore() = default;

    /// \brief Commits one envelope only after broker-proof validation.
    /// \param record Candidate broker allocation envelope.
    /// \param broker_store Durable source whose proof must authorize it.
    /// \return Commit, replay, conflict, validation, or I/O status.
    virtual BrokerAllocationCommitStatus commit(
        const BrokerAllocationEnvelope &record,
        const DurableBrokerReversalStore &broker_store) = 0;

    /// \brief Loads the envelope for one broker reversal.
    /// \param key Broker reversal identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    virtual BrokerAllocationLoadResult load(const BrokerReversalKey &key) const = 0;

    /// \brief Enumerates all envelopes for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    virtual BrokerAllocationScanResult scan() const = 0;
};

/// \brief Validates that a durable broker proof authorizes an envelope.
/// \param allocation Candidate broker allocation envelope.
/// \param broker_store Durable source of broker reversal proofs.
/// \return Proof-validation status.
inline BrokerAllocationProofStatus validate_broker_allocation_envelope(
    const BrokerAllocationEnvelope &allocation,
    const DurableBrokerReversalStore &broker_store) {
    if (!allocation.valid())
        return BrokerAllocationProofStatus::invalid_record;

    const auto broker = broker_store.load(allocation.broker_key);
    if (broker.status == BrokerReversalLoadStatus::not_found)
        return BrokerAllocationProofStatus::missing_broker_record;
    if (broker.status == BrokerReversalLoadStatus::invalid_record)
        return BrokerAllocationProofStatus::invalid_broker_record;
    if (broker.status == BrokerReversalLoadStatus::io_error)
        return BrokerAllocationProofStatus::io_error;
    if (!broker.found() || !broker.record || !broker.record->valid() ||
        !(broker.record->key() == allocation.broker_key) ||
        broker.record->account != allocation.account ||
        !(broker.record->provenance == allocation.broker_provenance) ||
        !(broker.record->broker_close_leg == allocation.broker_close_leg) ||
        !(broker.record->broker_reverse_open_leg == allocation.broker_reverse_open_leg) ||
        !(allocation.unallocated_close_leg == broker.record->broker_close_leg) ||
        !(allocation.unallocated_reverse_open_leg ==
          broker.record->broker_reverse_open_leg))
        return BrokerAllocationProofStatus::invalid_broker_record;

    return BrokerAllocationProofStatus::valid;
}

} // namespace mt5bridge

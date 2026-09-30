#pragma once

/// \file dispatch/journal_store.hpp
/// \brief Defines the durable store contract for operation journal records.

#include "journal_types.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge {

/// \enum StoreCommitStatus
/// \brief Reports the result of one compare-and-commit journal write.
enum class StoreCommitStatus {
    committed, ///< Candidate replaced the expected durable record.
    conflict,  ///< Expected revision/absence no longer matches durable state.
    io_error,  ///< The store could not durably commit the candidate.
};

/// \enum StoreLoadStatus
/// \brief Reports the result of loading one durable operation record.
enum class StoreLoadStatus {
    found,          ///< A complete valid record was loaded.
    not_found,      ///< No record exists for the requested key.
    invalid_record, ///< Storage exists but its record is malformed or inconsistent.
    io_error,       ///< The store could not complete the read.
};

/// \struct StoreLoadResult
/// \brief Carries a status-bearing single-record load result.
struct StoreLoadResult {
    StoreLoadStatus status = StoreLoadStatus::io_error;
    std::optional<OperationRecord> record;

    /// \brief Tests whether a complete record was loaded.
    /// \return True only when `record` is present and the status is `found`.
    bool found() const {
        return status == StoreLoadStatus::found && record.has_value();
    }
};

/// \enum StoreScanStatus
/// \brief Reports the result of enumerating durable operation records.
enum class StoreScanStatus {
    complete,       ///< Every operation record was loaded and validated.
    invalid_record, ///< At least one record is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \struct StoreScanResult
/// \brief Carries an all-or-nothing durable record enumeration.
struct StoreScanResult {
    StoreScanStatus status = StoreScanStatus::io_error;
    std::vector<OperationRecord> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when all discovered records are present and valid.
    bool complete() const { return status == StoreScanStatus::complete; }
};

/// \class DurableJournalStore
/// \brief Persistence seam whose compare-and-commit returns only after durable storage.
class DurableJournalStore {
public:
    virtual ~DurableJournalStore() = default;

    /// \brief Compare-and-commits one operation record durably.
    /// \param record Complete next record, including its incremented revision.
    /// \param expected_revision Expected current revision; empty means no record exists.
    /// \return Commit, conflict, or I/O status.
    /// \warning Returning `committed` before the record survives a process crash breaks
    /// the non-resendable dispatch barrier.
    virtual StoreCommitStatus commit(
        const OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) = 0;

    /// \brief Loads the last durable record for one operation.
    /// \param key Account-scoped operation identity.
    /// \return A status that distinguishes absence from invalid storage or I/O.
    virtual StoreLoadResult load(const OperationKey &key) const = 0;

    /// \brief Enumerates every durable operation record atomically for recovery.
    /// \return A complete record set, or a failure with no usable partial set.
    virtual StoreScanResult scan() const = 0;
};

} // namespace mt5bridge

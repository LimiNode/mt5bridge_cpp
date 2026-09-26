#pragma once

/// \file dispatch/file_journal_store.hpp
/// \brief Defines the Windows-backed durable operation journal store.

#include <mt5bridge/dispatch/journal.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace mt5bridge {

/// \class WindowsFileJournalStore
/// \brief Persists operation records as atomically replaced files on Windows.
///
/// The store serializes all records below one directory and uses a directory
/// lock file to make read-modify-write CAS operations single-writer across
/// processes. Each record is written to a temporary file, flushed, and then
/// atomically replaced with `MoveFileExW`. Malformed records are never
/// overwritten and are reported as a failed durable operation.
class WindowsFileJournalStore final : public DurableJournalStore {
public:
    /// \brief Opens or creates a journal directory.
    /// \param directory Directory containing operation records and the lock file.
    /// \note The directory is created when possible; a non-directory path leaves
    /// the store unavailable and all mutations fail closed.
    explicit WindowsFileJournalStore(std::filesystem::path directory);

    WindowsFileJournalStore(const WindowsFileJournalStore &) = delete;
    WindowsFileJournalStore &operator=(const WindowsFileJournalStore &) = delete;
    WindowsFileJournalStore(WindowsFileJournalStore &&) = delete;
    WindowsFileJournalStore &operator=(WindowsFileJournalStore &&) = delete;

    /// \brief Compares and durably replaces one operation record.
    /// \param record Complete candidate record with its next revision.
    /// \param expected_revision Expected durable revision, or empty for creation.
    /// \return `committed`, `conflict`, or `io_error`.
    StoreCommitStatus commit(const OperationRecord &record,
                             std::optional<std::uint64_t> expected_revision) override;

    /// \brief Loads one record after validating its complete serialized envelope.
    /// \param key Account-scoped operation identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    StoreLoadResult load(const OperationKey &key) const override;

    /// \brief Enumerates all operation records for process-restart recovery.
    /// \return An all-or-nothing record set; malformed entries invalidate the scan.
    StoreScanResult scan() const override;

    /// \brief Tests whether the directory was opened successfully.
    /// \return True when subsequent store operations can acquire the directory lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest diagnostic captured by this store.
    /// \return A stable diagnostic string until the next store operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Directory used for records and the lock file.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

/// \class WindowsSingleWriterLease
/// \brief Holds one account-scoped Windows ownership lock and fencing epoch.
///
/// Construction acquires an exclusive lock for the account and durably advances
/// that account's fencing epoch before reporting the lease ready. The lock is
/// held until destruction; a later owner therefore receives a strictly higher
/// token only after the previous owner has released the OS handle.
class WindowsSingleWriterLease final : public SingleWriterLease {
public:
    /// \brief Acquires the account-scoped lease below one journal directory.
    /// \param directory Directory shared by the journal and lease metadata.
    /// \param account Immutable server/login identity to fence.
    WindowsSingleWriterLease(std::filesystem::path directory, AccountKey account);
    ~WindowsSingleWriterLease() override;

    WindowsSingleWriterLease(const WindowsSingleWriterLease &) = delete;
    WindowsSingleWriterLease &operator=(const WindowsSingleWriterLease &) = delete;
    WindowsSingleWriterLease(WindowsSingleWriterLease &&) = delete;
    WindowsSingleWriterLease &operator=(WindowsSingleWriterLease &&) = delete;

    /// \brief Returns the held token only for the leased account.
    /// \param account Account identity being checked by the admission barrier.
    /// \return Current durable fencing token, or empty when not held.
    std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &account) const override;

    /// \brief Tests whether lock acquisition and epoch persistence succeeded.
    /// \return True while this object owns the account lease.
    bool ready() const { return ready_; }

    /// \brief Returns the latest construction or storage diagnostic.
    /// \return Stable diagnostic until destruction.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the anchored metadata directory.
    /// \return Absolute directory used for lock and epoch files.
    const std::filesystem::path &directory() const { return directory_; }

    /// \brief Returns the immutable account identity held by this lease.
    /// \return Account key supplied at construction.
    const AccountKey &account() const { return account_; }

private:
    struct State;

    std::filesystem::path directory_;
    AccountKey account_;
    std::unique_ptr<State> state_;
    bool ready_ = false;
    std::uint64_t token_ = 0;
    std::string last_error_;
};

} // namespace mt5bridge

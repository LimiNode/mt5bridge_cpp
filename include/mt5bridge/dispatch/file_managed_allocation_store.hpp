#pragma once

/// \file dispatch/file_managed_allocation_store.hpp
/// \brief Defines the Windows-backed managed-allocation store.

#include "managed_allocation.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \class WindowsFileManagedAllocationStore
/// \brief Persists one immutable managed allocation link per broker reversal.
///
/// Records use a dedicated namespace and lock. A duplicate identical link is
/// idempotent; assigning the same broker reversal to another managed operation
/// is a conflict. The store never chooses an ownership policy itself.
class WindowsFileManagedAllocationStore final : public DurableManagedAllocationStore {
public:
    /// \brief Opens or creates a managed-allocation directory.
    /// \param directory Directory containing allocation records and the lock file.
    explicit WindowsFileManagedAllocationStore(std::filesystem::path directory);

    WindowsFileManagedAllocationStore(const WindowsFileManagedAllocationStore &) = delete;
    WindowsFileManagedAllocationStore &operator=(
        const WindowsFileManagedAllocationStore &) = delete;
    WindowsFileManagedAllocationStore(WindowsFileManagedAllocationStore &&) = delete;
    WindowsFileManagedAllocationStore &operator=(WindowsFileManagedAllocationStore &&) = delete;

    /// \brief Commits one immutable allocation link atomically.
    /// \param record Complete managed allocation link.
    /// \param broker_store Durable source whose proof must authorize the link.
    /// \return Commit, replay, conflict, validation, or I/O status.
    ManagedAllocationCommitStatus commit(
        const ManagedAllocationRecord &record,
        const DurableBrokerReversalStore &broker_store) override;

    /// \brief Loads an allocation by its broker reversal identity.
    /// \param key Broker reversal key.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    ManagedAllocationLoadResult load(const BrokerReversalKey &key) const override;

    /// \brief Enumerates every allocation link for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    ManagedAllocationScanResult scan() const override;

    /// \brief Tests whether the directory was opened successfully.
    /// \return True when operations can acquire the store lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next store operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Directory used for records and the lock file.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge

#pragma once

/// \file dispatch/file_managed_ownership_basis_store.hpp
/// \brief Defines the Windows-backed durable managed-ownership-basis store.

#include "managed_ownership_basis.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \class WindowsFileManagedOwnershipBasisStore
/// \brief Persists immutable operation-scoped ownership upper bounds.
class WindowsFileManagedOwnershipBasisStore final
    : public DurableManagedOwnershipBasisStore {
public:
    /// \brief Opens or creates a basis directory.
    /// \param directory Directory containing basis records and the lock file.
    explicit WindowsFileManagedOwnershipBasisStore(std::filesystem::path directory);

    WindowsFileManagedOwnershipBasisStore(
        const WindowsFileManagedOwnershipBasisStore &) = delete;
    WindowsFileManagedOwnershipBasisStore &operator=(
        const WindowsFileManagedOwnershipBasisStore &) = delete;
    WindowsFileManagedOwnershipBasisStore(WindowsFileManagedOwnershipBasisStore &&) = delete;
    WindowsFileManagedOwnershipBasisStore &operator=(
        WindowsFileManagedOwnershipBasisStore &&) = delete;

    /// \brief Commits one proof-gated ownership basis atomically.
    /// \param record Candidate basis.
    /// \param journal_store Durable operation source.
    /// \param allocation_store Durable broker-envelope source.
    /// \return Commit, replay, conflict, validation, or I/O status.
    ManagedOwnershipBasisCommitStatus commit(
        const ManagedOwnershipBasis &record,
        const DurableJournalStore &journal_store,
        const DurableBrokerAllocationStore &allocation_store) override;

    /// \brief Loads one basis by its composite identity.
    /// \param key Broker reversal and operation identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    ManagedOwnershipBasisLoadResult load(
        const ManagedOwnershipBasisKey &key) const override;

    /// \brief Enumerates every basis for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    ManagedOwnershipBasisScanResult scan() const override;

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

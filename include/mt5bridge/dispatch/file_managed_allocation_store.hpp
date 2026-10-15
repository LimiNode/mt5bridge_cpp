#pragma once

/// \file dispatch/file_managed_allocation_store.hpp
/// \brief Defines the Windows-backed durable managed allocation store.

#include "managed_allocation.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \class WindowsFileManagedAllocationStore
/// \brief Persists immutable close-leg allocations under one namespace lock.
class WindowsFileManagedAllocationStore final : public DurableManagedAllocationStore {
public:
    /// \brief Opens or creates an allocation directory.
    /// \param directory Directory containing allocation records and lock file.
    explicit WindowsFileManagedAllocationStore(std::filesystem::path directory);

    WindowsFileManagedAllocationStore(const WindowsFileManagedAllocationStore &) = delete;
    WindowsFileManagedAllocationStore &operator=(
        const WindowsFileManagedAllocationStore &) = delete;
    WindowsFileManagedAllocationStore(WindowsFileManagedAllocationStore &&) = delete;
    WindowsFileManagedAllocationStore &operator=(WindowsFileManagedAllocationStore &&) = delete;

    /// \brief Commits or replays one source-coherent allocation entry.
    ManagedAllocationCommitStatus commit(
        const ManagedAllocationEntry &entry,
        const ManagedBrokerVolumeMapping &mapping,
        const DurableJournalStore &journal_store,
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const SingleWriterLease &lease) override;

    /// \brief Commits an allocation for an exact staged settlement recipe.
    ManagedAllocationCommitStatus commit_staged(
        const ManagedAllocationEntry &entry,
        const ManagedBrokerVolumeMapping &mapping,
        const ManagedSettlementPreparation &preparation,
        const DurableJournalStore &journal_store,
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const SingleWriterLease &lease) override;

    /// \brief Loads one entry by exact composite identity.
    ManagedAllocationLoadResult load(const ManagedAllocationKey &key) const override;

    /// \brief Scans and historically validates every allocation entry.
    ManagedAllocationScanResult scan(
        const DurableManagedDealSettlementStore &settlement_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store) const override;

    /// \brief Tests whether the directory was opened successfully.
    /// \return True when the store can acquire its namespace lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next store operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Canonical allocation directory.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge

#pragma once

/// \file dispatch/file_managed_deal_settlement_store.hpp
/// \brief Defines the Windows-backed managed deal settlement store.

#include "managed_deal_settlement.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \enum ManagedDealSettlementScanStatus
/// \brief Reports all-or-nothing settlement-store enumeration.
enum class ManagedDealSettlementScanStatus { complete, invalid_record, io_error };

/// \struct ManagedDealSettlementScanResult
/// \brief Carries committed facts, pending facts, and frontiers recovered after restart.
struct ManagedDealSettlementScanResult {
    ManagedDealSettlementScanStatus status = ManagedDealSettlementScanStatus::io_error;
    std::vector<ManagedDealSettlement> facts; ///< Facts referenced by a committed frontier.
    std::vector<ManagedSettlementFrontier> frontiers;
    std::vector<ManagedDealSettlement> pending_facts; ///< Valid facts awaiting publication.

    /// \brief Tests whether the complete durable set was recovered.
    /// \return True only when no record was malformed or unreadable.
    bool complete() const { return status == ManagedDealSettlementScanStatus::complete; }
};

/// \class WindowsFileManagedDealSettlementStore
/// \brief Persists immutable per-deal facts and operation frontiers.
class WindowsFileManagedDealSettlementStore final
    : public DurableManagedDealSettlementStore {
public:
    /// \brief Opens or creates a settlement directory.
    /// \param directory Directory containing fact/frontier records and lock file.
    explicit WindowsFileManagedDealSettlementStore(std::filesystem::path directory);

    WindowsFileManagedDealSettlementStore(const WindowsFileManagedDealSettlementStore &) = delete;
    WindowsFileManagedDealSettlementStore &operator=(
        const WindowsFileManagedDealSettlementStore &) = delete;
    WindowsFileManagedDealSettlementStore(WindowsFileManagedDealSettlementStore &&) = delete;
    WindowsFileManagedDealSettlementStore &operator=(
        WindowsFileManagedDealSettlementStore &&) = delete;

    /// \brief Commits one fact and complete frontier under the store lock.
    /// \param proof Sealed producer output containing fact and frontier.
    /// \param journal_store Durable source operation store.
    /// \return Proof, replay, conflict, validation, or I/O status.
    ManagedDealSettlementCommitStatus commit(
        const ManagedDealSettlementCommit &proof,
        const DurableJournalStore &journal_store) override;

    /// \brief Completes durable publication manifests left by an earlier process.
    /// \param journal_store Durable source operation store used to revalidate each pair.
    /// \return Recovery, no-pending, validation, or I/O status.
    ManagedDealSettlementCommitStatus recover_pending(
        const DurableJournalStore &journal_store);

    /// \brief Loads one immutable deal fact by composite identity.
    /// \param key Operation/deal identity.
    /// \return Status-bearing lookup result.
    ManagedDealSettlementLoadResult load(
        const ManagedDealSettlementKey &key) const override;

    /// \brief Scans all fact and frontier files for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    ManagedDealSettlementScanResult scan() const;

    /// \brief Tests whether the directory is usable.
    /// \return True when the store can acquire its lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Directory used for records and locking.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge

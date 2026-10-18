#pragma once

/// \file dispatch/file_managed_settlement_preparation_store.hpp
/// \brief Defines the Windows-backed staged settlement publication store.

#include "managed_settlement_preparation.hpp"
#include "managed_allocation.hpp"
#include "file_managed_deal_settlement_store.hpp"
#include "file_managed_ownership_basis_store.hpp"
#include "file_broker_allocation_store.hpp"
#include "file_broker_reversal_store.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace mt5bridge {

/// \enum ManagedSettlementPreparationScanStatus
/// \brief Reports the result of enumerating staged settlement manifests.
enum class ManagedSettlementPreparationScanStatus { complete, invalid_record, io_error };

/// \struct ManagedSettlementPreparationScanResult
/// \brief Carries all valid staged manifests discovered after restart.
struct ManagedSettlementPreparationScanResult {
    ManagedSettlementPreparationScanStatus status =
        ManagedSettlementPreparationScanStatus::io_error;
    std::vector<ManagedSettlementPreparation> records;

    /// \brief Tests whether every manifest was decoded and validated.
    /// \return True only for a complete scan.
    bool complete() const {
        return status == ManagedSettlementPreparationScanStatus::complete;
    }
};

/// \class WindowsFileManagedSettlementPreparationStore
/// \brief Persists exact staged settlement recipes until finalization.
class WindowsFileManagedSettlementPreparationStore final
    : public DurableManagedSettlementPreparationStore {
public:
    /// \brief Opens or creates a preparation directory.
    /// \param directory Directory containing staged manifests and lock file.
    explicit WindowsFileManagedSettlementPreparationStore(std::filesystem::path directory);

    WindowsFileManagedSettlementPreparationStore(
        const WindowsFileManagedSettlementPreparationStore &) = delete;
    WindowsFileManagedSettlementPreparationStore &operator=(
        const WindowsFileManagedSettlementPreparationStore &) = delete;

    /// \brief Durably records one exact target and staged ownership association.
    /// \param proof Sealed authoritative settlement fact/frontier.
    /// \param target_operation Exact operation candidate for the target revision.
    /// \param base_operation_revision Current journal revision expected by CAS.
    /// \param staged_ownership Ownership association retained until finalization.
    /// \param journal_store Durable source used to validate the base operation.
    /// \return Preparation, replay, conflict, validation, or I/O status.
    ManagedSettlementPreparationStatus prepare(
        const ManagedDealSettlementCommit &proof,
        const OperationRecord &target_operation,
        std::uint64_t base_operation_revision,
        const ManagedStagedOwnershipAssociation &staged_ownership,
        const DurableJournalStore &journal_store);

    /// \brief Loads one exact durable preparation manifest.
    /// \param operation_key Operation identity addressed by the manifest.
    /// \param operation_revision Exact prepared target revision.
    /// \param deal_ticket Deal identity addressed by the manifest.
    /// \return Status-bearing durable preparation result.
    ManagedSettlementPreparationLoadResult load(
        const OperationKey &operation_key, std::uint64_t operation_revision,
        std::uint64_t deal_ticket) const override;

    /// \brief Finalizes one prepared recipe and its staged ownership basis.
    /// \param preparation Exact recipe returned by the preparation step.
    /// \param journal_store Mutable journal used for the guarded target CAS.
    /// \param settlement_store Durable committed settlement store.
    /// \param basis_store Durable ordinary ownership-basis store.
    /// \param envelope_store Durable broker-envelope source.
    /// \param broker_store Durable broker reversal source.
    /// \param allocation_store Durable allocation ledger used as the publication gate.
    /// \param lease Authoritative account-scoped writer ownership.
    /// \return Finalization, pending, conflict, validation, or I/O status.
    ManagedSettlementFinalizeStatus finalize(
        const ManagedSettlementPreparation &preparation,
        DurableJournalStore &journal_store,
        DurableManagedDealSettlementStore &settlement_store,
        DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedAllocationStore &allocation_store,
        const SingleWriterLease &lease);

    /// \brief Recovers every pending manifest using only durable state.
    /// \param journal_store Mutable journal used for guarded target CAS.
    /// \param settlement_store Durable committed settlement store.
    /// \param basis_store Durable ordinary ownership-basis store.
    /// \param envelope_store Durable broker-envelope source.
    /// \param broker_store Durable broker reversal source.
    /// \param allocation_store Durable allocation ledger publication gate.
    /// \param lease Authoritative account-scoped writer ownership.
    /// \return Finalization status for the recovered manifest set.
    ManagedSettlementFinalizeStatus recover_pending(
        DurableJournalStore &journal_store,
        DurableManagedDealSettlementStore &settlement_store,
        DurableManagedOwnershipBasisStore &basis_store,
        const DurableBrokerAllocationStore &envelope_store,
        const DurableBrokerReversalStore &broker_store,
        const DurableManagedAllocationStore &allocation_store,
        const SingleWriterLease &lease);

    /// \brief Scans staged manifests without applying any mutation.
    /// \return Complete manifests, or an all-or-nothing failure.
    ManagedSettlementPreparationScanResult scan() const;

    /// \brief Tests whether the store opened successfully.
    /// \return True when the directory can be locked.
    bool ready() const { return ready_; }

    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next operation.
    const std::string &last_error() const { return last_error_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge

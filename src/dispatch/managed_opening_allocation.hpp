#pragma once

/// \file dispatch/managed_opening_allocation.hpp
/// \brief Defines the durable source-derived OPEN allocation boundary.

#include "managed_opening_execution_evidence.hpp"
#include "managed_opening_provenance_contract.hpp"

#include <mt5bridge/dispatch/journal_store.hpp>
#include <mt5bridge/dispatch/lease.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mt5bridge::dispatch {

class DurableManagedOpeningFactStore;
class ManagedOpeningSourceVerifier;
class VerifiedManagedOpeningSourceFact;
class WindowsFileManagedOpeningAllocationStore;

/// \struct ManagedOpeningAllocationFactReference
/// \brief Pins one durable B3A fact into an immutable opening frontier.
struct ManagedOpeningAllocationFactReference {
    std::uint64_t deal_ticket = 0; ///< Stable broker execution identity.
    std::uint64_t source_revision = 0; ///< Fact admission history revision.
    std::uint64_t graph_instance_id = 0; ///< Observation provenance instance.
    std::uint64_t history_coverage_revision = 0; ///< Exact covered revision.
    std::uint32_t schema_version = ManagedOpeningDealFact::k_schema_version;

    /// \brief Tests the durable reference shape.
    /// \return True only for one complete B3A source reference.
    bool valid() const {
        return deal_ticket != 0 && source_revision != 0 && graph_instance_id != 0 &&
               history_coverage_revision == source_revision &&
               schema_version == ManagedOpeningDealFact::k_schema_version;
    }

    /// \brief Compares every durable reference field.
    /// \param other Reference to compare.
    /// \return True when both references are identical.
    bool operator==(const ManagedOpeningAllocationFactReference &other) const {
        return deal_ticket == other.deal_ticket && source_revision == other.source_revision &&
               graph_instance_id == other.graph_instance_id &&
               history_coverage_revision == other.history_coverage_revision &&
               schema_version == other.schema_version;
    }
};

/// \struct ManagedOpeningAllocationKey
/// \brief Identifies one immutable cumulative opening frontier snapshot.
struct ManagedOpeningAllocationKey {
    OperationKey operation_key; ///< Managed OPEN identity.
    std::uint64_t operation_revision = 0; ///< Immutable result-source revision.
    std::uint64_t frontier_revision = 0; ///< Latest admitted history revision.

    /// \brief Tests the account-scoped frontier identity.
    /// \return True only for a complete allocation key.
    bool valid() const {
        return operation_key.valid() && operation_revision != 0 && frontier_revision != 0;
    }

    /// \brief Compares exact frontier identity.
    /// \param other Key to compare.
    /// \return True when all key fields match.
    bool operator==(const ManagedOpeningAllocationKey &other) const {
        return operation_key == other.operation_key &&
               operation_revision == other.operation_revision &&
               frontier_revision == other.frontier_revision;
    }
};

/// \struct ManagedOpeningAllocationEntry
/// \brief Immutable source-derived allocation for one cumulative OPEN frontier.
struct ManagedOpeningAllocationEntry {
    AccountKey account; ///< Account namespace shared by every source.
    ManagedOpeningAllocationKey key; ///< OPEN and frontier identity.
    std::uint64_t position_identifier = 0; ///< Broker POSITION_IDENTIFIER.
    std::uint64_t order_ticket = 0; ///< Broker order joined to every deal.
    BrokerPositionDirection direction = BrokerPositionDirection::unknown;
    std::uint32_t quantity_scale = 0; ///< Canonical decimal quantity scale.
    std::uint64_t broker_step_units = 0; ///< Canonical broker step numerator.
    std::uint64_t managed_logical_units_per_step = 0; ///< Reviewed unit mapping.
    std::uint64_t cumulative_broker_units = 0; ///< Exact cumulative broker units.
    std::uint64_t cumulative_managed_logical_units = 0; ///< Exact managed units.
    std::uint64_t evidence_digest = 0; ///< B3B source-chain digest.
    std::vector<ManagedOpeningAllocationFactReference> fact_references;

    /// \brief Tests identity and quantity invariants independent of source reload.
    /// \return True only for a complete immutable allocation shape.
    bool valid() const {
        if (!account.valid() || !key.valid() || account != key.operation_key.account ||
            position_identifier == 0 || order_ticket == 0 ||
            !valid_broker_position_direction(direction) || quantity_scale > 9 ||
            broker_step_units == 0 || managed_logical_units_per_step == 0 ||
            cumulative_broker_units == 0 || cumulative_managed_logical_units == 0 ||
            evidence_digest == 0 || fact_references.empty())
            return false;
        for (std::size_t index = 0; index != fact_references.size(); ++index) {
            const auto &reference = fact_references[index];
            if (!reference.valid() ||
                reference.source_revision > key.frontier_revision ||
                (index != 0 && reference.deal_ticket <=
                                   fact_references[index - 1].deal_ticket))
                return false;
        }
        return true;
    }

    /// \brief Compares the complete immutable allocation payload.
    /// \param other Entry to compare.
    /// \return True when every persisted field matches.
    bool operator==(const ManagedOpeningAllocationEntry &other) const {
        return account == other.account && key == other.key &&
               position_identifier == other.position_identifier &&
               order_ticket == other.order_ticket && direction == other.direction &&
               quantity_scale == other.quantity_scale &&
               broker_step_units == other.broker_step_units &&
               managed_logical_units_per_step == other.managed_logical_units_per_step &&
               cumulative_broker_units == other.cumulative_broker_units &&
               cumulative_managed_logical_units == other.cumulative_managed_logical_units &&
               evidence_digest == other.evidence_digest &&
               fact_references == other.fact_references;
    }
};

/// \struct ManagedOpeningAllocationPreparation
/// \brief Durable retry manifest for one exact opening allocation publication.
struct ManagedOpeningAllocationPreparation {
    ManagedOpeningAllocationEntry entry;

    /// \brief Tests whether the retry manifest has a complete entry.
    /// \return True only for a valid immutable allocation candidate.
    bool valid() const { return entry.valid(); }

    /// \brief Compares exact preparation content.
    /// \param other Preparation to compare.
    /// \return True when both manifests contain the same entry.
    bool operator==(const ManagedOpeningAllocationPreparation &other) const {
        return entry == other.entry;
    }
};

/// \enum ManagedOpeningAllocationCommitStatus
/// \brief Reports one durable opening allocation publication decision.
enum class ManagedOpeningAllocationCommitStatus : std::uint8_t {
    committed = 0,
    already_committed = 1,
    conflict = 2,
    invalid_record = 3,
    missing_source = 4,
    invalid_source = 5,
    lease_not_held = 6,
    io_error = 7,
};

/// \enum ManagedOpeningAllocationLoadStatus
/// \brief Reports one allocation or preparation lookup result.
enum class ManagedOpeningAllocationLoadStatus : std::uint8_t {
    found = 0,
    not_found = 1,
    invalid_record = 2,
    io_error = 3,
};

/// \enum ManagedOpeningAllocationScanStatus
/// \brief Reports an all-or-nothing account allocation scan.
enum class ManagedOpeningAllocationScanStatus : std::uint8_t {
    complete = 0,
    invalid_record = 1,
    io_error = 2,
};

/// \struct ManagedOpeningAllocationLoadResult
/// \brief Carries one status-bearing allocation lookup.
struct ManagedOpeningAllocationLoadResult {
    ManagedOpeningAllocationLoadStatus status =
        ManagedOpeningAllocationLoadStatus::io_error;
    std::optional<ManagedOpeningAllocationEntry> record;

    /// \brief Tests whether a valid allocation was loaded.
    /// \return True only for a found record.
    bool found() const {
        return status == ManagedOpeningAllocationLoadStatus::found && record.has_value();
    }
};

/// \struct ManagedOpeningAllocationPreparationLoadResult
/// \brief Carries one status-bearing retry-manifest lookup.
struct ManagedOpeningAllocationPreparationLoadResult {
    ManagedOpeningAllocationLoadStatus status =
        ManagedOpeningAllocationLoadStatus::io_error;
    std::optional<ManagedOpeningAllocationPreparation> record;

    /// \brief Tests whether a valid preparation was loaded.
    /// \return True only for a found manifest.
    bool found() const {
        return status == ManagedOpeningAllocationLoadStatus::found && record.has_value();
    }
};

/// \struct ManagedOpeningAllocationScanResult
/// \brief Carries all valid allocations and pending preparations for an account.
struct ManagedOpeningAllocationScanResult {
    ManagedOpeningAllocationScanStatus status = ManagedOpeningAllocationScanStatus::io_error;
    std::vector<ManagedOpeningAllocationEntry> records;
    std::vector<ManagedOpeningAllocationPreparation> preparations;

    /// \brief Tests whether the account namespace was scanned completely.
    /// \return True only when no durable record was skipped or malformed.
    bool complete() const {
        return status == ManagedOpeningAllocationScanStatus::complete;
    }
};

/// \class DurableManagedOpeningAllocationStore
/// \brief Persistence seam for immutable source-derived opening allocations.
class DurableManagedOpeningAllocationStore {
public:
    virtual ~DurableManagedOpeningAllocationStore() = default;

    /// \brief Persists or replays one exact preparation manifest.
    virtual ManagedOpeningAllocationCommitStatus prepare(
        const ManagedOpeningAllocationEntry &entry,
        const VerifiedManagedOpeningSourceFact &proof,
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const SingleWriterLease &lease) = 0;

    /// \brief Loads one exact durable preparation manifest.
    virtual ManagedOpeningAllocationPreparationLoadResult load_preparation(
        const ManagedOpeningAllocationKey &key) const = 0;

    /// \brief Publishes or replays an allocation at the authoritative B3B boundary.
    /// \note Implementations must invoke the supplied verifier while the
    ///       account fence is held; the caller proof is never sufficient by
    ///       itself for durable allocation publication.
    virtual ManagedOpeningAllocationCommitStatus publish(
        const ManagedOpeningAllocationPreparation &preparation,
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const SingleWriterLease &lease,
        const ManagedOpeningSourceVerifier &verifier,
        const ManagedOpeningProvenanceRequest &request) = 0;

    /// \brief Loads one immutable allocation by exact frontier identity.
    virtual ManagedOpeningAllocationLoadResult load(
        const ManagedOpeningAllocationKey &key) const = 0;

    /// \brief Scans committed allocations and validates their durable sources.
    virtual ManagedOpeningAllocationScanResult scan(
        const AccountKey &account, const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store) const = 0;
};

/// \enum ManagedOpeningAllocationPublicationStatus
/// \brief Reports the source-derived publisher boundary.
enum class ManagedOpeningAllocationPublicationStatus : std::uint8_t {
    prepared = 0,
    already_prepared = 1,
    published = 2,
    already_published = 3,
    invalid_request = 4,
    unavailable = 5,
    invalid_evidence = 6,
    conflict = 7,
    lease_not_held = 8,
    io_error = 9,
};

/// \struct ManagedOpeningAllocationPublicationResult
/// \brief Carries a source-derived allocation publication outcome.
struct ManagedOpeningAllocationPublicationResult {
    ManagedOpeningAllocationPublicationStatus status =
        ManagedOpeningAllocationPublicationStatus::unavailable;
    std::optional<ManagedOpeningAllocationEntry> record;
};

/// \class ManagedOpeningAllocationPublisher
/// \brief Re-verifies B3B sources before preparing and publishing B3C allocation.
class ManagedOpeningAllocationPublisher final {
public:
    /// \brief Binds one journal, B3A fact store, B3B verifier, and allocation store.
    ManagedOpeningAllocationPublisher(
        const ManagedOpeningSourceVerifier &verifier,
        const DurableManagedOpeningFactStore &fact_store,
        DurableManagedOpeningAllocationStore &allocation_store,
        const DurableJournalStore &journal_store, const SingleWriterLease &lease)
        : verifier_(verifier), fact_store_(fact_store), allocation_store_(allocation_store),
          journal_store_(journal_store), lease_(lease) {}

    /// \brief Verifies sources and persists a retryable allocation preparation.
    /// \param request Current durable journal revision and position query.
    /// \return Preparation outcome with its immutable candidate when available.
    ManagedOpeningAllocationPublicationResult prepare(
        const ManagedOpeningProvenanceRequest &request) const;

    /// \brief Reloads a preparation, re-verifies sources, and publishes it.
    /// \param key Exact prepared frontier identity.
    /// \param request Current journal revision and position query.
    /// \return Publication or explicit fail-closed outcome.
    ManagedOpeningAllocationPublicationResult publish(
        const ManagedOpeningAllocationKey &key,
        const ManagedOpeningProvenanceRequest &request) const;

private:
    const ManagedOpeningSourceVerifier &verifier_;
    const DurableManagedOpeningFactStore &fact_store_;
    DurableManagedOpeningAllocationStore &allocation_store_;
    const DurableJournalStore &journal_store_;
    const SingleWriterLease &lease_;
};

/// \class WindowsFileManagedOpeningAllocationStore
/// \brief Persists immutable opening frontiers with checksum and fencing.
class WindowsFileManagedOpeningAllocationStore final
    : public DurableManagedOpeningAllocationStore {
public:
    /// \brief Opens or creates the allocation namespace.
    /// \param directory Directory containing allocation records and manifests.
    explicit WindowsFileManagedOpeningAllocationStore(std::filesystem::path directory);

    WindowsFileManagedOpeningAllocationStore(
        const WindowsFileManagedOpeningAllocationStore &) = delete;
    WindowsFileManagedOpeningAllocationStore &operator=(
        const WindowsFileManagedOpeningAllocationStore &) = delete;
    WindowsFileManagedOpeningAllocationStore(WindowsFileManagedOpeningAllocationStore &&) = delete;
    WindowsFileManagedOpeningAllocationStore &operator=(
        WindowsFileManagedOpeningAllocationStore &&) = delete;

    ManagedOpeningAllocationCommitStatus prepare(
        const ManagedOpeningAllocationEntry &entry,
        const VerifiedManagedOpeningSourceFact &proof,
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const SingleWriterLease &lease) override;
    ManagedOpeningAllocationPreparationLoadResult load_preparation(
        const ManagedOpeningAllocationKey &key) const override;
    ManagedOpeningAllocationCommitStatus publish(
        const ManagedOpeningAllocationPreparation &preparation,
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const SingleWriterLease &lease,
        const ManagedOpeningSourceVerifier &verifier,
        const ManagedOpeningProvenanceRequest &request) override;
    ManagedOpeningAllocationLoadResult load(
        const ManagedOpeningAllocationKey &key) const override;
    ManagedOpeningAllocationScanResult scan(
        const AccountKey &account, const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store) const override;

    /// \brief Tests whether the directory was opened successfully.
    /// \return True when filesystem operations are available.
    bool ready() const { return ready_; }
    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next operation.
    const std::string &last_error() const { return last_error_; }
    /// \brief Returns the canonical storage directory.
    /// \return Configured directory.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge::dispatch

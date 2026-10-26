#pragma once

/// \file dispatch/managed_opening_fact_store.hpp
/// \brief Defines the private durable store contract for OPEN deal facts.

#include "managed_opening_execution_evidence.hpp"

#include <mt5bridge/dispatch/lease.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mt5bridge::dispatch {

/// \enum ManagedOpeningFactCommitStatus
/// \brief Reports one durable per-deal publication decision.
enum class ManagedOpeningFactCommitStatus : std::uint8_t {
    committed = 0,
    already_committed = 1,
    conflict = 2,
    invalid_record = 3,
    lease_not_held = 4,
    io_error = 5,
};

/// \enum ManagedOpeningFactLoadStatus
/// \brief Reports one durable per-deal lookup result.
enum class ManagedOpeningFactLoadStatus : std::uint8_t {
    found = 0,
    not_found = 1,
    invalid_record = 2,
    io_error = 3,
};

/// \struct ManagedOpeningFactLoadResult
/// \brief Carries a status-bearing immutable fact lookup.
struct ManagedOpeningFactLoadResult {
    ManagedOpeningFactLoadStatus status = ManagedOpeningFactLoadStatus::io_error;
    std::optional<ManagedOpeningDealFact> fact;

    /// \brief Tests whether a valid fact was recovered.
    /// \return True only when status is found and a fact is present.
    bool found() const {
        return status == ManagedOpeningFactLoadStatus::found && fact.has_value();
    }
};

/// \enum ManagedOpeningFactScanStatus
/// \brief Reports an all-or-nothing durable fact scan.
enum class ManagedOpeningFactScanStatus : std::uint8_t {
    complete = 0,
    invalid_record = 1,
    io_error = 2,
};

/// \struct ManagedOpeningFactScanResult
/// \brief Carries facts recovered from one account namespace.
struct ManagedOpeningFactScanResult {
    ManagedOpeningFactScanStatus status = ManagedOpeningFactScanStatus::io_error;
    std::vector<ManagedOpeningDealFact> facts;

    /// \brief Tests whether every scanned record was valid.
    /// \return True only for a complete scan.
    bool complete() const {
        return status == ManagedOpeningFactScanStatus::complete;
    }
};

/// \class DurableManagedOpeningFactStore
/// \brief Persistence seam for locally validated immutable OPEN facts.
class DurableManagedOpeningFactStore {
public:
    virtual ~DurableManagedOpeningFactStore() = default;

    /// \brief Publishes or replays one fact under account fencing.
    /// \param fact Locally validated fact; this call does not establish source authority.
    /// \param lease Account-scoped lease held across the atomic publication.
    /// \return Durable publication decision.
    virtual ManagedOpeningFactCommitStatus commit(
        const ManagedOpeningDealFact &fact,
        const SingleWriterLease &lease) = 0;

    /// \brief Loads one fact by account and stable deal ticket.
    /// \param account Account namespace.
    /// \param deal_ticket Stable broker execution identity.
    /// \return Status-bearing lookup result.
    virtual ManagedOpeningFactLoadResult load(
        const AccountKey &account, std::uint64_t deal_ticket) const = 0;

    /// \brief Recovers every fact in one account namespace.
    /// \param account Account namespace to scan.
    /// \return Complete facts or an all-or-nothing failure.
    virtual ManagedOpeningFactScanResult scan(const AccountKey &account) const = 0;
};

/// \class WindowsFileManagedOpeningFactStore
/// \brief Stores canonical OPEN facts in flushed, atomically replaced files.
class WindowsFileManagedOpeningFactStore final : public DurableManagedOpeningFactStore {
public:
    /// \brief Opens or creates the fact directory.
    /// \param directory Directory containing fact records and its lock file.
    explicit WindowsFileManagedOpeningFactStore(std::filesystem::path directory);

    WindowsFileManagedOpeningFactStore(const WindowsFileManagedOpeningFactStore &) = delete;
    WindowsFileManagedOpeningFactStore &operator=(
        const WindowsFileManagedOpeningFactStore &) = delete;
    WindowsFileManagedOpeningFactStore(WindowsFileManagedOpeningFactStore &&) = delete;
    WindowsFileManagedOpeningFactStore &operator=(WindowsFileManagedOpeningFactStore &&) = delete;

    ManagedOpeningFactCommitStatus commit(
        const ManagedOpeningDealFact &fact,
        const SingleWriterLease &lease) override;
    ManagedOpeningFactLoadResult load(
        const AccountKey &account, std::uint64_t deal_ticket) const override;
    ManagedOpeningFactScanResult scan(const AccountKey &account) const override;

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

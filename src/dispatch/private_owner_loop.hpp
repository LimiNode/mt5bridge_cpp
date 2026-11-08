#pragma once

/// \file private_owner_loop.hpp
/// \brief Declares the private startup and recovery owner-loop coordinator.

#include <mt5bridge/dispatch/operation_recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace mt5bridge::dispatch {

/// \enum PrivateOwnerLoopStartupStatus
/// \brief Reports whether durable owner-loop startup completed.
enum class PrivateOwnerLoopStartupStatus {
    ready,             ///< Lease and complete journal recovery succeeded.
    lease_unavailable, ///< The account-scoped lease could not be held.
    recovery_failed,   ///< Durable journal recovery was incomplete or invalid.
};

/// \struct PrivateOwnerLoopStartupResult
/// \brief Describes one startup attempt.
struct PrivateOwnerLoopStartupResult {
    PrivateOwnerLoopStartupStatus status =
        PrivateOwnerLoopStartupStatus::recovery_failed;
    JournalMutationStatus recovery_status = JournalMutationStatus::invalid_record;
    std::size_t operation_count = 0;

    /// \brief Tests whether startup permits caller-driven owner steps.
    /// \return True only after lease and complete recovery succeeded.
    bool ready() const { return status == PrivateOwnerLoopStartupStatus::ready; }
};

/// \enum PrivateOwnerLoopStepStatus
/// \brief Reports one bounded, caller-driven recovery outcome.
enum class PrivateOwnerLoopStepStatus {
    startup_required,     ///< Startup has not completed successfully.
    lease_lost,            ///< The startup fencing capability is no longer held.
    recovery_failed,      ///< A fresh complete journal scan failed.
    idle,                 ///< No non-terminal durable operation remains.
    reconcile_only,       ///< Observation may progress this operation; never resend.
    resume_pre_dispatch,  ///< Caller may apply normal admission/fencing rules.
};

/// \struct PrivateOwnerLoopStepResult
/// \brief Carries one recovered operation and its safe next action.
struct PrivateOwnerLoopStepResult {
    PrivateOwnerLoopStepStatus status =
        PrivateOwnerLoopStepStatus::startup_required;
    std::optional<RecoveredOperation> operation;

    /// \brief Tests whether the operation may ever be resent by this loop.
    /// \return False for every step that crosses or observes a dispatch barrier.
    bool non_resendable() const {
        return status == PrivateOwnerLoopStepStatus::reconcile_only ||
               (operation && operation->non_resendable());
    }
};

/// \enum PrivateOwnerLoopIntentStatus
/// \brief Reports whether a new intent may enter this owner scope.
enum class PrivateOwnerLoopIntentStatus {
    accepted,         ///< The scope is ready and has no unresolved records.
    not_ready,        ///< Startup has not completed or the lease was lost.
    account_mismatch, ///< The intent belongs to another account.
    duplicate,        ///< The operation key already exists durably.
    unresolved_block,  ///< A recovered non-terminal operation still blocks.
};

/// \struct PrivateOwnerLoopIntentResult
/// \brief Reports fail-closed intent admission without creating a record.
struct PrivateOwnerLoopIntentResult {
    PrivateOwnerLoopIntentStatus status =
        PrivateOwnerLoopIntentStatus::not_ready;

    /// \brief Tests whether the caller may perform its normal prepare path.
    /// \return True only when this coordinator found no owner-loop blocker.
    bool accepted() const {
        return status == PrivateOwnerLoopIntentStatus::accepted;
    }
};

/// \class PrivateOwnerLoop
/// \brief Coordinates account lease, durable recovery, and explicit owner steps.
///
/// The coordinator is intentionally caller-driven. It never invokes a broker
/// transport or Python; reconcile_only is handed to the caller's observation
/// worker. Every fresh step rescans the durable journal, so a worker's
/// terminal transition is visible without a second scheduler or event bus.
class PrivateOwnerLoop final {
public:
    /// \brief Binds one account to one durable journal and lease.
    /// \param journal Journal shared with the owner and admission barrier.
    /// \param account Immutable account scope for this owner.
    /// \param lease Account-scoped single-writer fencing capability.
    PrivateOwnerLoop(OperationJournal &journal, AccountKey account,
                     const SingleWriterLease &lease)
        : journal_(journal), account_(std::move(account)), lease_(lease) {}

    PrivateOwnerLoop(const PrivateOwnerLoop &) = delete;
    PrivateOwnerLoop &operator=(const PrivateOwnerLoop &) = delete;

    /// \brief Acquires the lease fence and performs complete journal recovery.
    /// \return Startup outcome and the number of recovered records.
    PrivateOwnerLoopStartupResult start();

    /// \brief Rechecks the lease and returns one caller-driven recovery action.
    /// \return Explicit action; never invokes a transport or mutates a record.
    PrivateOwnerLoopStepResult step();

    /// \brief Tests whether a new operation may enter this owner scope.
    /// \param key Account-scoped operation identity to prepare.
    /// \return Fail-closed admission decision; no durable record is created.
    PrivateOwnerLoopIntentResult intent_admission(const OperationKey &key);

    /// \brief Tests whether startup completed and the lease remains held.
    /// \return True while normal caller-driven steps may proceed.
    bool ready() const;

private:
    /// \brief Refreshes the complete durable classification.
    /// \return Recovery result or a failure outcome.
    OperationRecoveryResult refresh_recovery();

    /// \brief Verifies that the startup fencing token is still authoritative.
    /// \return True only while the same lease token is held.
    bool lease_held() const;

    OperationJournal &journal_;
    AccountKey account_;
    const SingleWriterLease &lease_;
    std::optional<std::uint64_t> fencing_token_;
    bool started_ = false;
};

} // namespace mt5bridge::dispatch

/// \file private_owner_loop.cpp
/// \brief Implements the private startup and recovery owner-loop coordinator.

#include "private_owner_loop.hpp"

namespace mt5bridge::dispatch {

PrivateOwnerLoopStartupResult PrivateOwnerLoop::start() {
    started_ = false;
    fencing_token_.reset();
    if (!account_.valid())
        return {PrivateOwnerLoopStartupStatus::lease_unavailable,
                JournalMutationStatus::invalid_record, 0};

    fencing_token_ = lease_.held_fencing_token(account_);
    if (!fencing_token_ || *fencing_token_ == 0)
        return {PrivateOwnerLoopStartupStatus::lease_unavailable,
                JournalMutationStatus::not_durable, 0};

    const auto recovered = refresh_recovery();
    if (recovered.status != JournalMutationStatus::accepted)
        return {PrivateOwnerLoopStartupStatus::recovery_failed,
                recovered.status, 0};

    started_ = true;
    return {PrivateOwnerLoopStartupStatus::ready, recovered.status,
            recovered.operations.size()};
}

PrivateOwnerLoopStepResult PrivateOwnerLoop::step() {
    if (!started_)
        return {PrivateOwnerLoopStepStatus::startup_required, std::nullopt};
    if (!lease_held())
        return {PrivateOwnerLoopStepStatus::lease_lost, std::nullopt};

    const auto recovered = refresh_recovery();
    if (recovered.status != JournalMutationStatus::accepted)
        return {PrivateOwnerLoopStepStatus::recovery_failed, std::nullopt};

    for (const auto &operation : recovered.operations) {
        if (operation.action == OperationRecoveryAction::terminal)
            continue;
        if (operation.action == OperationRecoveryAction::reconcile_only)
            return {PrivateOwnerLoopStepStatus::reconcile_only, operation};
        return {PrivateOwnerLoopStepStatus::resume_pre_dispatch, operation};
    }
    return {PrivateOwnerLoopStepStatus::idle, std::nullopt};
}

PrivateOwnerLoopIntentResult PrivateOwnerLoop::intent_admission(
    const OperationKey &key) {
    if (!ready())
        return {PrivateOwnerLoopIntentStatus::not_ready};
    if (!key.valid() || key.account != account_)
        return {PrivateOwnerLoopIntentStatus::account_mismatch};
    if (journal_.find(key))
        return {PrivateOwnerLoopIntentStatus::duplicate};

    const auto recovered = refresh_recovery();
    if (recovered.status != JournalMutationStatus::accepted)
        return {PrivateOwnerLoopIntentStatus::not_ready};
    for (const auto &operation : recovered.operations) {
        if (operation.action != OperationRecoveryAction::terminal)
            return {PrivateOwnerLoopIntentStatus::unresolved_block};
    }
    return {PrivateOwnerLoopIntentStatus::accepted};
}

bool PrivateOwnerLoop::ready() const {
    return started_ && lease_held();
}

OperationRecoveryResult PrivateOwnerLoop::refresh_recovery() {
    return OperationRecoveryCoordinator::recover(journal_);
}

bool PrivateOwnerLoop::lease_held() const {
    return fencing_token_ && *fencing_token_ != 0 &&
           lease_.held_fencing_token(account_) == fencing_token_;
}

} // namespace mt5bridge::dispatch

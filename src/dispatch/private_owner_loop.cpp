/// \file private_owner_loop.cpp
/// \brief Implements the private startup and recovery owner-loop coordinator.

#include "private_owner_loop.hpp"

#include <algorithm>
#include <vector>

namespace mt5bridge::dispatch {

PrivateOwnerLoopStartupResult PrivateOwnerLoop::start() {
    started_ = false;
    fencing_token_.reset();
    cursor_.reset();
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
    if (!lease_held())
        return {PrivateOwnerLoopStartupStatus::lease_unavailable,
                JournalMutationStatus::not_durable, 0};

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
    if (!lease_held())
        return {PrivateOwnerLoopStepStatus::lease_lost, std::nullopt};

    std::vector<const RecoveredOperation *> pending;
    for (const auto &operation : recovered.operations) {
        if (operation.record.key.account == account_ && blocks_scope(operation))
            pending.push_back(&operation);
    }
    if (pending.empty()) {
        cursor_.reset();
        return {PrivateOwnerLoopStepStatus::idle, std::nullopt};
    }

    std::size_t selected = 0;
    if (cursor_) {
        const auto after = std::find_if(
            pending.begin(), pending.end(), [this](const auto *operation) {
                return cursor_.value() < operation->record.key;
            });
        if (after != pending.end())
            selected = static_cast<std::size_t>(after - pending.begin());
    }
    const auto &operation = *pending[selected];
    cursor_ = operation.record.key;
    if (operation.record.operation_state == OperationState::ambiguous)
        return {PrivateOwnerLoopStepStatus::ambiguous_blocked, operation};
    if (operation.action == OperationRecoveryAction::reconcile_only)
        return {PrivateOwnerLoopStepStatus::reconcile_only, operation};
    return {PrivateOwnerLoopStepStatus::resume_pre_dispatch, operation};
}

PrivateOwnerLoopIntentResult PrivateOwnerLoop::intent_admission(
    const OperationKey &key) {
    if (!ready())
        return {PrivateOwnerLoopIntentStatus::not_ready};
    if (!key.valid() || key.account != account_)
        return {PrivateOwnerLoopIntentStatus::account_mismatch};
    const auto recovered = refresh_recovery();
    if (recovered.status != JournalMutationStatus::accepted)
        return {PrivateOwnerLoopIntentStatus::not_ready};
    if (!lease_held())
        return {PrivateOwnerLoopIntentStatus::not_ready};
    for (const auto &operation : recovered.operations) {
        if (operation.record.key.account != account_)
            continue;
        if (operation.record.key == key)
            return {PrivateOwnerLoopIntentStatus::duplicate};
        if (blocks_scope(operation))
            return {PrivateOwnerLoopIntentStatus::unresolved_block};
    }
    return {PrivateOwnerLoopIntentStatus::accepted};
}

PrivateOwnerLoopDispatchResult PrivateOwnerLoop::dispatch_eligibility(
    const OperationKey &key) {
    if (!ready())
        return {PrivateOwnerLoopDispatchStatus::not_ready};
    if (!key.valid() || key.account != account_)
        return {PrivateOwnerLoopDispatchStatus::account_mismatch};

    const auto recovered = refresh_recovery();
    if (recovered.status != JournalMutationStatus::accepted || !lease_held())
        return {PrivateOwnerLoopDispatchStatus::not_ready};

    const RecoveredOperation *requested = nullptr;
    for (const auto &operation : recovered.operations) {
        if (operation.record.key.account != account_)
            continue;
        if (operation.record.key == key) {
            requested = &operation;
            continue;
        }
        if (blocks_scope(operation))
            return {PrivateOwnerLoopDispatchStatus::blocked};
    }
    if (!requested || requested->action != OperationRecoveryAction::resume_pre_dispatch)
        return {requested ? PrivateOwnerLoopDispatchStatus::blocked
                          : PrivateOwnerLoopDispatchStatus::operation_missing};
    return {PrivateOwnerLoopDispatchStatus::accepted};
}

bool PrivateOwnerLoop::ready() const {
    return started_ && lease_held();
}

OperationRecoveryResult PrivateOwnerLoop::refresh_recovery() {
    return OperationRecoveryCoordinator::recover(journal_);
}

bool PrivateOwnerLoop::blocks_scope(const RecoveredOperation &operation) const {
    return operation.action != OperationRecoveryAction::terminal ||
           operation.record.operation_state == OperationState::ambiguous;
}

bool PrivateOwnerLoop::lease_held() const {
    return fencing_token_ && *fencing_token_ != 0 &&
           lease_.held_fencing_token(account_) == fencing_token_;
}

} // namespace mt5bridge::dispatch

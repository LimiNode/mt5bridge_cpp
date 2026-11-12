/// \file private_trade_manager.cpp
/// \brief Implements the bounded private OPEN manager composition.

#include "private_trade_manager.hpp"

#include <utility>

namespace mt5bridge::dispatch {

PrivateTradeManager::PrivateTradeManager(
    managed_trade::ManagedTradeState initial, AccountKey account,
    OperationJournal &journal, DispatchAdmissionBarrier &admission,
    runtime::OneShotDispatchBackend &backend,
    runtime::CurrentAccountProbe &account_probe, const SingleWriterLease &lease,
    runtime::DispatchTransport &transport, ObservationCoordinator &coordinator,
    ObservationCollectionRequest collection)
    : initial_(std::move(initial)), account_(std::move(account)), journal_(journal),
      admission_(admission), backend_(backend), account_probe_(account_probe),
      lease_(lease), transport_(transport), coordinator_(coordinator),
      collection_(std::move(collection)), loop_(journal_, account_, lease_) {}

PrivateTradeManagerResult PrivateTradeManager::start() {
    owner_.reset();
    const auto startup = loop_.start();
    if (!startup.ready())
        return {startup.status == PrivateOwnerLoopStartupStatus::lease_unavailable
                    ? PrivateTradeManagerStatus::lease_lost
                    : PrivateTradeManagerStatus::recovery_failed, std::nullopt};
    if (!initial_.valid() || !account_.valid() ||
        initial_.slice.operation_id != 0 || initial_.plan.slice_count != 0 ||
        initial_.open_volume != 0 || initial_.pending_remainder_volume != 0 ||
        initial_.slice.state != managed_trade::OperationState::idle)
        return {PrivateTradeManagerStatus::invalid_state, std::nullopt};

    const auto recovery = journal_.recover_all();
    if (!recovery.accepted() || !loop_.ready())
        return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};

    const OperationRecord *own_record = nullptr;
    for (const auto &record : recovery.records()) {
        if (record.key.account == account_ &&
            record.key.trade_id == initial_.trade_id.value) {
            if (own_record)
                return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
            own_record = &record;
        }
    }
    auto restored = initial_;
    if (own_record) {
        if (own_record->key.operation_id != 1 || !own_record->valid() ||
            own_record->operation_kind != OperationKind::open ||
            own_record->settlement_mode != ManagedSettlementMode::legacy ||
            !own_record->reconciliation_descriptor ||
            own_record->reconciliation_descriptor->requested_volume == 0)
            return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
        if (own_record->journal_state == JournalState::dispatch_intent_persisted &&
            own_record->operation_state == OperationState::prechecking) {
            const auto fresh_baseline = coordinator_.capture_baseline();
            if (!fresh_baseline.valid() ||
                journal_.rebase_pre_dispatch_descriptor(
                    own_record->key, fresh_baseline)
                        .status != JournalMutationStatus::accepted)
                return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
            if (!own_record->result_payload.empty() ||
                !own_record->reconciliation_bindings.empty() ||
                own_record->settled_volume != 0 || own_record->fencing_token != 0 ||
                restored.start_open_slice(
                    own_record->reconciliation_descriptor->requested_volume) !=
                    managed_trade::MutationStatus::applied)
                return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
        } else {
            auto pending = ManagedTradeOwner::recover_pending_open(initial_, account_,
                                                                    recovery);
            if (pending) {
                restored = std::move(*pending);
            } else {
                auto settled = ManagedTradeOwner::recover_settled_open(
                    initial_, account_, recovery);
                if (!settled)
                    return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
                restored = std::move(*settled);
            }
        }
    }
    owner_ = std::make_unique<ManagedTradeOwner>(
        std::move(restored), account_, journal_, admission_, backend_, account_probe_,
        lease_, transport_);
    return {PrivateTradeManagerStatus::ready, std::nullopt};
}

PrivateTradeManagerResult PrivateTradeManager::prepare_open(
    managed_trade::Volume volume, ManagedTradeIntent intent) {
    if (!owner_)
        return {PrivateTradeManagerStatus::stopped, std::nullopt};
    if (!loop_.ready())
        return {PrivateTradeManagerStatus::lease_lost, std::nullopt};
    const auto &current = owner_->state();
    if (current.slice.operation_id != 0)
        return {PrivateTradeManagerStatus::invalid_state, std::nullopt};
    const OperationKey key{account_, current.trade_id.value,
                           current.slice.operation_id + 1};
    const auto admission = loop_.intent_admission(key);
    if (admission.status != PrivateOwnerLoopIntentStatus::accepted)
        return {admission.status == PrivateOwnerLoopIntentStatus::not_ready
                    ? PrivateTradeManagerStatus::lease_lost
                    : PrivateTradeManagerStatus::blocked, std::nullopt};
    return {PrivateTradeManagerStatus::owner_result,
            owner_->prepare_open(volume, std::move(intent))};
}

PrivateTradeManagerResult PrivateTradeManager::check_action(
    const OperationKey &key, PrivateOwnerLoopStepStatus required) {
    if (!owner_)
        return {PrivateTradeManagerStatus::stopped, std::nullopt};
    const auto action = loop_.step();
    if (action.status == PrivateOwnerLoopStepStatus::lease_lost)
        return {PrivateTradeManagerStatus::lease_lost, std::nullopt};
    if (action.status == PrivateOwnerLoopStepStatus::startup_required)
        return {PrivateTradeManagerStatus::stopped, std::nullopt};
    if (action.status == PrivateOwnerLoopStepStatus::recovery_failed)
        return {PrivateTradeManagerStatus::recovery_failed, std::nullopt};
    if (action.status != required || !action.operation ||
        action.operation->record.key != key)
        return {PrivateTradeManagerStatus::blocked, std::nullopt};
    return {PrivateTradeManagerStatus::ready, std::nullopt};
}

PrivateTradeManagerResult PrivateTradeManager::execute_pending(
    const DispatchAdmissionRequest &request) {
    if (!owner_)
        return {PrivateTradeManagerStatus::stopped, std::nullopt};
    const auto &state = owner_->state();
    const OperationKey key{account_, state.trade_id.value,
                           state.slice.operation_id};
    const auto eligible = loop_.dispatch_eligibility(key);
    if (!eligible.accepted())
        return {eligible.status == PrivateOwnerLoopDispatchStatus::not_ready
                    ? PrivateTradeManagerStatus::lease_lost
                    : PrivateTradeManagerStatus::blocked,
                std::nullopt};
    const auto check = check_action(key,
                                    PrivateOwnerLoopStepStatus::resume_pre_dispatch);
    if (check.status != PrivateTradeManagerStatus::ready)
        return check;
    return {PrivateTradeManagerStatus::owner_result,
            owner_->execute_pending(request)};
}

PrivateTradeManagerResult PrivateTradeManager::reconcile(bool trade_event_gap,
                                                          bool deadline_expired) {
    if (!owner_)
        return {PrivateTradeManagerStatus::stopped, std::nullopt};
    const auto &state = owner_->state();
    const OperationKey key{account_, state.trade_id.value,
                           state.slice.operation_id};
    const auto check = check_action(key, PrivateOwnerLoopStepStatus::reconcile_only);
    if (check.status != PrivateTradeManagerStatus::ready)
        return check;
    if (state.slice.kind != managed_trade::OperationKind::open)
        return {PrivateTradeManagerStatus::invalid_state, std::nullopt};
    OperationReconciliationWorker worker(journal_, key, coordinator_, collection_);
    if (state.slice.state == managed_trade::OperationState::partially_filled)
        return {PrivateTradeManagerStatus::owner_result,
                owner_->settle_pending_remainder(worker, trade_event_gap,
                                                  deadline_expired)};
    if (state.slice.state != managed_trade::OperationState::submitting)
        return {PrivateTradeManagerStatus::invalid_state, std::nullopt};
    return {PrivateTradeManagerStatus::owner_result,
            owner_->settle_reconciliation(worker, trade_event_gap, deadline_expired)};
}

const managed_trade::ManagedTradeState *PrivateTradeManager::state() const {
    return owner_ && loop_.ready() ? &owner_->state() : nullptr;
}

void PrivateTradeManager::shutdown() { owner_.reset(); }

} // namespace mt5bridge::dispatch

/// \file dispatch/managed_trade_owner.cpp
/// \brief Implements the private managed-trade owner-loop bridge.

#include "managed_trade_owner.hpp"

#include <mt5bridge/reconciliation/graph.hpp>
#include <mt5bridge/trade/deals.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {
namespace {

bool is_result_pending(const OperationRecord &record) {
    return record.journal_state == JournalState::result_persisted ||
           record.journal_state == JournalState::reconciling;
}

} // namespace

ManagedTradeOwner::ManagedTradeOwner(
    managed_trade::ManagedTradeState initial_state, AccountKey account,
    OperationJournal &journal, DispatchAdmissionBarrier &admission,
    runtime::OneShotDispatchBackend &backend,
    runtime::CurrentAccountProbe &account_probe, const SingleWriterLease &lease,
    runtime::DispatchTransport &transport)
    : state_(std::move(initial_state)), account_(std::move(account)), journal_(journal),
      admission_(admission), backend_(backend), account_probe_(account_probe),
      lease_(lease), transport_(transport) {}

std::optional<managed_trade::ManagedTradeState>
ManagedTradeOwner::recover_settled_open(
    managed_trade::ManagedTradeState initial_state, const AccountKey &account,
    const JournalRecoveryResult &recovery) {
    if (!recovery.accepted() || !account.valid() || !initial_state.valid() ||
        initial_state.slice.operation_id != 0 || initial_state.plan.slice_count != 0 ||
        initial_state.open_volume != 0 || initial_state.pending_remainder_volume != 0 ||
        initial_state.slice.state != managed_trade::OperationState::idle)
        return std::nullopt;

    std::vector<const OperationRecord *> settled;
    for (const auto &record : recovery.records()) {
        if (record.key.account != account ||
            record.key.trade_id != initial_state.trade_id.value)
            continue;
        if (!record.valid() || record.journal_state != JournalState::reconciling ||
            record.operation_kind != OperationKind::open ||
            !record.reconciliation_descriptor ||
            record.reconciliation_descriptor->requested_volume == 0 ||
            record.settled_volume == 0 ||
            (record.operation_state != OperationState::partially_filled &&
             record.operation_state != OperationState::filled))
            return std::nullopt;
        settled.push_back(&record);
    }
    std::sort(settled.begin(), settled.end(),
              [](const auto *left, const auto *right) {
                  return left->key.operation_id < right->key.operation_id;
              });

    auto candidate = std::move(initial_state);
    for (const auto *record : settled) {
        const auto requested = record->reconciliation_descriptor->requested_volume;
        if (candidate.slice.operation_id == (std::numeric_limits<std::uint64_t>::max)() ||
            record->key.operation_id != candidate.slice.operation_id + 1 ||
            requested == 0 || requested > candidate.plan.max_slice_volume ||
            record->settled_volume > requested)
            return std::nullopt;
        const auto expected_state =
            record->settled_volume == requested
                ? OperationState::filled
                : OperationState::partially_filled;
        if (record->operation_state != expected_state ||
            candidate.start_open_slice(requested) !=
                managed_trade::MutationStatus::applied ||
            candidate.enter_submitting() !=
                managed_trade::MutationStatus::applied ||
            candidate.record_fill(record->settled_volume) !=
                managed_trade::MutationStatus::applied ||
            candidate.reconcile() != managed_trade::MutationStatus::applied ||
            candidate.slice.operation_id != record->key.operation_id ||
            !candidate.valid())
            return std::nullopt;
    }
    return candidate;
}

std::optional<managed_trade::ManagedTradeState>
ManagedTradeOwner::recover_settled_trade(
    managed_trade::ManagedTradeState initial_state, const AccountKey &account,
    const JournalRecoveryResult &recovery) {
    if (!recovery.accepted() || !account.valid() || !initial_state.valid() ||
        initial_state.slice.operation_id != 0 || initial_state.plan.slice_count != 0 ||
        initial_state.open_volume != 0 || initial_state.pending_remainder_volume != 0 ||
        initial_state.slice.state != managed_trade::OperationState::idle)
        return std::nullopt;

    std::vector<const OperationRecord *> records;
    for (const auto &record : recovery.records()) {
        if (record.key.account != account ||
            record.key.trade_id != initial_state.trade_id.value)
            continue;
        if (!record.valid() || record.journal_state != JournalState::reconciling ||
            !record.reconciliation_descriptor || record.settled_volume >
                                                       record.reconciliation_descriptor
                                                           ->requested_volume)
            return std::nullopt;
        if (record.operation_kind == OperationKind::open) {
            if (record.reconciliation_descriptor->requested_volume == 0 ||
                record.settled_volume == 0 ||
                (record.operation_state != OperationState::partially_filled &&
                 record.operation_state != OperationState::filled))
                return std::nullopt;
        } else if (record.operation_kind == OperationKind::close) {
            if (record.reconciliation_descriptor->requested_volume == 0 ||
                record.settled_volume == 0 ||
                (record.operation_state != OperationState::partially_filled &&
                 record.operation_state != OperationState::filled))
                return std::nullopt;
        } else if (record.operation_kind == OperationKind::cancel) {
            if (record.reconciliation_descriptor->requested_volume == 0 ||
                record.operation_state != OperationState::cancelled ||
                record.settled_volume != 0)
                return std::nullopt;
        } else {
            return std::nullopt;
        }
        records.push_back(&record);
    }
    std::sort(records.begin(), records.end(),
              [](const auto *left, const auto *right) {
                  return left->key.operation_id < right->key.operation_id;
              });

    auto candidate = std::move(initial_state);
    for (const auto *record : records) {
        if (candidate.slice.operation_id ==
                (std::numeric_limits<std::uint64_t>::max)() ||
            record->key.operation_id != candidate.slice.operation_id + 1)
            return std::nullopt;

        const auto requested = record->reconciliation_descriptor->requested_volume;
        managed_trade::MutationStatus status = managed_trade::MutationStatus::invalid_state;
        switch (record->operation_kind) {
        case OperationKind::open:
            if (requested == 0 || requested > candidate.plan.max_slice_volume ||
                record->settled_volume > requested ||
                record->operation_state !=
                    (record->settled_volume == requested ? OperationState::filled
                                                          : OperationState::partially_filled) ||
                candidate.start_open_slice(requested) !=
                    managed_trade::MutationStatus::applied ||
                candidate.enter_submitting() != managed_trade::MutationStatus::applied ||
                candidate.record_fill(record->settled_volume) !=
                    managed_trade::MutationStatus::applied)
                return std::nullopt;
            status = candidate.reconcile();
            break;
        case OperationKind::close:
            if (requested == 0 || requested > candidate.open_volume ||
                record->settled_volume > requested ||
                record->operation_state !=
                    (record->settled_volume == requested ? OperationState::filled
                                                          : OperationState::partially_filled) ||
                (!candidate.close_obligation.requested &&
                 candidate.request_close() != managed_trade::MutationStatus::applied) ||
                candidate.start_close(requested) !=
                    managed_trade::MutationStatus::applied ||
                candidate.enter_submitting() != managed_trade::MutationStatus::applied ||
                candidate.record_fill(record->settled_volume) !=
                    managed_trade::MutationStatus::applied)
                return std::nullopt;
            status = candidate.reconcile();
            break;
        case OperationKind::cancel:
            if (candidate.pending_remainder_volume == 0 ||
                record->operation_state != OperationState::cancelled ||
                candidate.start_cancel() != managed_trade::MutationStatus::applied ||
                candidate.enter_submitting() != managed_trade::MutationStatus::applied ||
                candidate.record_cancel_accepted() !=
                    managed_trade::MutationStatus::applied)
                return std::nullopt;
            status = candidate.reconcile();
            break;
        case OperationKind::unspecified:
            return std::nullopt;
        }
        if (status != managed_trade::MutationStatus::applied ||
            candidate.slice.operation_id != record->key.operation_id ||
            !candidate.valid())
            return std::nullopt;
    }
    return candidate;
}

std::optional<OperationKey> ManagedTradeOwner::current_key() const {
    if (!state_.trade_id.valid() || state_.slice.operation_id == 0 ||
        !account_.valid())
        return std::nullopt;
    return OperationKey{account_, state_.trade_id.value, state_.slice.operation_id};
}

OwnerStepResult ManagedTradeOwner::result_for(
    OwnerStepStatus status, std::optional<OperationKey> key,
    std::optional<OperationRecord> record) const {
    OwnerStepResult result;
    result.status = status;
    result.key = std::move(key);
    result.record = std::move(record);
    return result;
}

OwnerStepResult ManagedTradeOwner::prepare_durable_operation(
    managed_trade::ManagedTradeState candidate, OperationKind kind,
    ManagedTradeIntent intent) {
    intent.reconciliation_descriptor.requested_volume =
        candidate.slice.requested_volume;
    if (!account_.valid() || !candidate.valid() || intent.request_payload.empty() ||
        !intent.reconciliation_descriptor.valid() ||
        intent.reconciliation_descriptor.account != account_ ||
        (kind == OperationKind::open &&
         candidate.slice.kind != managed_trade::OperationKind::open) ||
        (kind == OperationKind::close &&
         candidate.slice.kind != managed_trade::OperationKind::close) ||
        (kind == OperationKind::cancel &&
         candidate.slice.kind != managed_trade::OperationKind::cancel) ||
        ((kind == OperationKind::open || kind == OperationKind::close) &&
         intent.reconciliation_descriptor.settled_state != OperationState::filled &&
         intent.reconciliation_descriptor.settled_state !=
             OperationState::partially_filled) ||
        (kind == OperationKind::cancel &&
         intent.reconciliation_descriptor.settled_state != OperationState::cancelled))
        return result_for(OwnerStepStatus::invalid_request);

    const OperationKey key{account_, candidate.trade_id.value,
                           candidate.slice.operation_id};
    if (journal_.find(key))
        return result_for(OwnerStepStatus::invalid_state, key, journal_.find(key));

    const auto created = journal_.create(
        key, std::move(intent.request_payload), kind);
    if (!created.accepted())
        return result_for(OwnerStepStatus::durable_failure, key, created.record);
    const auto prechecking = journal_.transition_operation(
        key, OperationState::prechecking);
    if (!prechecking.accepted())
        return result_for(OwnerStepStatus::durable_failure, key, prechecking.record);
    const auto prechecked = journal_.transition_journal(key, JournalState::prechecked);
    if (!prechecked.accepted())
        return result_for(OwnerStepStatus::durable_failure, key, prechecked.record);
    const auto intent_persisted = journal_.transition_journal(
        key, JournalState::dispatch_intent_persisted);
    if (!intent_persisted.accepted())
        return result_for(OwnerStepStatus::durable_failure, key,
                          intent_persisted.record);
    const auto descriptor = journal_.persist_reconciliation_descriptor(
        key, std::move(intent.reconciliation_descriptor));
    if (!descriptor.accepted())
        return result_for(OwnerStepStatus::durable_failure, key, descriptor.record);
    state_ = std::move(candidate);
    return result_for(OwnerStepStatus::prepared, key, descriptor.record);
}

OwnerStepResult ManagedTradeOwner::prepare_open(managed_trade::Volume volume,
                                                ManagedTradeIntent intent) {
    if (!account_.valid() || !state_.valid() || intent.request_payload.empty() ||
        !intent.reconciliation_descriptor.valid() ||
        intent.reconciliation_descriptor.account != account_)
        return result_for(OwnerStepStatus::invalid_request);
    auto candidate = state_;
    if (candidate.start_open_slice(volume) != managed_trade::MutationStatus::applied)
        return result_for(OwnerStepStatus::invalid_state);
    return prepare_durable_operation(std::move(candidate), OperationKind::open,
                                     std::move(intent));
}

OwnerStepResult ManagedTradeOwner::prepare_close(managed_trade::Volume volume,
                                                 ManagedTradeIntent intent) {
    if (!account_.valid() || !state_.valid() || intent.request_payload.empty() ||
        !intent.reconciliation_descriptor.valid() ||
        intent.reconciliation_descriptor.account != account_)
        return result_for(OwnerStepStatus::invalid_request);
    auto candidate = state_;
    if (!candidate.close_obligation.requested &&
        candidate.request_close() != managed_trade::MutationStatus::applied)
        return result_for(OwnerStepStatus::invalid_state);
    if (candidate.start_close(volume) != managed_trade::MutationStatus::applied)
        return result_for(OwnerStepStatus::invalid_state);
    return prepare_durable_operation(std::move(candidate), OperationKind::close,
                                     std::move(intent));
}

OwnerStepResult ManagedTradeOwner::prepare_cancel(ManagedTradeIntent intent) {
    if (!account_.valid() || !state_.valid() || intent.request_payload.empty() ||
        !intent.reconciliation_descriptor.valid() ||
        intent.reconciliation_descriptor.account != account_)
        return result_for(OwnerStepStatus::invalid_request);
    auto candidate = state_;
    if (candidate.start_cancel() != managed_trade::MutationStatus::applied)
        return result_for(OwnerStepStatus::invalid_state);
    return prepare_durable_operation(std::move(candidate), OperationKind::cancel,
                                     std::move(intent));
}

OwnerStepResult ManagedTradeOwner::execute_pending(
    const DispatchAdmissionRequest &request) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.state != managed_trade::OperationState::dispatching)
        return result_for(OwnerStepStatus::invalid_state, current_key());
    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    auto admitted = admission_.admit(*key, request, lease_);
    if (!admitted.admitted()) {
        auto result = result_for(OwnerStepStatus::admission_rejected, key,
                                journal_.find(*key));
        result.admission_status = admitted.status;
        return result;
    }

    auto executed = backend_.execute(journal_, *key, std::move(*admitted.permit),
                                      account_probe_, lease_, transport_);
    auto record = executed.record ? executed.record : journal_.find(*key);
    auto result = result_for(OwnerStepStatus::execution_failed, key, record);
    result.execution_status = executed.status;
    result.retcode = executed.retcode;

    if (executed.status == runtime::OneShotExecutionStatus::completed && record) {
        if (record->operation_state == OperationState::rejected) {
            auto candidate = state_;
            if (!state_to_submitting(&candidate) ||
                candidate.record_rejected() != managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                return result;
            state_ = std::move(candidate);
            result.status = OwnerStepStatus::completed;
            return result;
        }
        if (record->operation_state == OperationState::accepted ||
            record->operation_state == OperationState::reconciling) {
            auto candidate = state_;
            if (!state_to_submitting(&candidate))
                return result;
            state_ = std::move(candidate);
            result.status = OwnerStepStatus::awaiting_reconciliation;
            return result;
        }
    }

    const bool barrier_reached = record &&
                                 record->journal_state == JournalState::dispatching &&
                                 record->operation_state == OperationState::submitting;
    const bool result_needs_reconciliation =
        record && is_result_pending(*record) && !record->result_payload.empty();
    const bool uncertain_after_barrier =
        barrier_reached || executed.status == runtime::OneShotExecutionStatus::transport_failure ||
        executed.status == runtime::OneShotExecutionStatus::result_not_durable ||
        executed.status == runtime::OneShotExecutionStatus::reconciliation_binding_failed ||
        (executed.status == runtime::OneShotExecutionStatus::account_mismatch &&
         record && record->operation_state == OperationState::submitting) ||
        (executed.status == runtime::OneShotExecutionStatus::lease_lost && barrier_reached);

    if (result_needs_reconciliation &&
        executed.status == runtime::OneShotExecutionStatus::lifecycle_transition_failed) {
        auto candidate = state_;
        if (!state_to_submitting(&candidate))
            return result;
        state_ = std::move(candidate);
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    }

    if (uncertain_after_barrier) {
        if (!mark_journal_reconciling(*key)) {
            result.status = OwnerStepStatus::durable_failure;
            result.record = journal_.find(*key);
            return result;
        }
        auto candidate = state_;
        if (!state_to_submitting(&candidate))
            return result;
        state_ = std::move(candidate);
        result.status = OwnerStepStatus::ambiguous;
        result.record = journal_.find(*key);
    }
    return result;
}

OwnerStepResult ManagedTradeOwner::settle_reconciliation(
    OperationReconciliationWorker &worker, bool trade_event_gap,
    bool deadline_expired) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.state != managed_trade::OperationState::submitting)
        return result_for(OwnerStepStatus::invalid_state, current_key());
    if (state_.slice.kind != managed_trade::OperationKind::open)
        return result_for(OwnerStepStatus::invalid_state, current_key());

    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    if (worker.key() != *key)
        return result_for(OwnerStepStatus::invalid_request, key,
                          journal_.find(*key));
    const auto current = journal_.find(*key);
    if (!current || !current->reconciliation_descriptor)
        return result_for(OwnerStepStatus::invalid_state, key, current);
    const auto settled_state = current->reconciliation_descriptor->settled_state;
    if (settled_state != OperationState::filled &&
        settled_state != OperationState::partially_filled)
        return result_for(OwnerStepStatus::invalid_state, key, current);

    const auto cycle = worker.step(trade_event_gap, deadline_expired);
    auto result = result_for(OwnerStepStatus::durable_failure, key, cycle.record);
    if (!result.record)
        result.record = journal_.find(*key);

    switch (cycle.status) {
    case OperationReconciliationStatus::pending:
    case OperationReconciliationStatus::not_observed:
    case OperationReconciliationStatus::trade_event_gap:
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    case OperationReconciliationStatus::account_mismatch:
    case OperationReconciliationStatus::ambiguous:
        if (!cycle.record ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record)) {
            result.status = OwnerStepStatus::ambiguous;
            return result;
        }
        if (cycle.record->operation_state != OperationState::ambiguous)
            return result;
        {
            auto candidate = state_;
            if (candidate.record_unknown() != managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                return result;
            state_ = std::move(candidate);
        }
        result.status = OwnerStepStatus::ambiguous;
        return result;
    case OperationReconciliationStatus::invalid_request:
        result.status = OwnerStepStatus::invalid_request;
        return result;
    case OperationReconciliationStatus::operation_not_found:
    case OperationReconciliationStatus::invalid_state:
        result.status = OwnerStepStatus::invalid_state;
        return result;
    case OperationReconciliationStatus::persistence_failed:
        return result;
    case OperationReconciliationStatus::confirmed:
        if (!cycle.record ||
            cycle.record->operation_state != OperationState::reconciling ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record))
            break;
        {
            const auto executed = history_deal_volume(
                cycle, worker, *cycle.record, DealDirection::entry);
            if (!executed || *executed == 0 || state_.slice.requested_volume == 0)
                break;

            if (*executed > state_.slice.requested_volume)
                break;
            const auto applied_volume = *executed;
            const auto target_state =
                *executed == state_.slice.requested_volume
                    ? OperationState::filled
                    : OperationState::partially_filled;
            auto candidate = state_;
            if (candidate.record_fill(applied_volume) !=
                    managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                break;
            const auto transitioned =
                journal_.transition_operation(*key, target_state, *executed);
            if (!transitioned.accepted())
                break;
            state_ = std::move(candidate);
            result.record = transitioned.record;
            result.status = target_state == OperationState::filled
                                 ? OwnerStepStatus::completed
                                 : OwnerStepStatus::partially_filled;
            return result;
        }
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    }

    if (cycle.status == OperationReconciliationStatus::confirmed)
        result.status = OwnerStepStatus::awaiting_reconciliation;
    return result;
}

OwnerStepResult ManagedTradeOwner::settle_pending_remainder(
    OperationReconciliationWorker &worker, bool trade_event_gap,
    bool deadline_expired) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.kind != managed_trade::OperationKind::open ||
        state_.slice.state != managed_trade::OperationState::partially_filled ||
        state_.pending_remainder_volume == 0)
        return result_for(OwnerStepStatus::invalid_state, current_key());

    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    if (worker.key() != *key)
        return result_for(OwnerStepStatus::invalid_request, key,
                          journal_.find(*key));
    const auto current = journal_.find(*key);
    if (!current || current->journal_state != JournalState::reconciling ||
        current->operation_state != OperationState::partially_filled ||
        !current->reconciliation_descriptor)
        return result_for(OwnerStepStatus::invalid_state, key, current);

    const auto cycle = worker.step(trade_event_gap, deadline_expired);
    auto result = result_for(OwnerStepStatus::durable_failure, key, cycle.record);
    if (!result.record)
        result.record = journal_.find(*key);

    switch (cycle.status) {
    case OperationReconciliationStatus::pending:
    case OperationReconciliationStatus::not_observed:
    case OperationReconciliationStatus::trade_event_gap:
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    case OperationReconciliationStatus::account_mismatch:
    case OperationReconciliationStatus::ambiguous:
        result.status = OwnerStepStatus::ambiguous;
        return result;
    case OperationReconciliationStatus::invalid_request:
        result.status = OwnerStepStatus::invalid_request;
        return result;
    case OperationReconciliationStatus::operation_not_found:
    case OperationReconciliationStatus::invalid_state:
        result.status = OwnerStepStatus::invalid_state;
        return result;
    case OperationReconciliationStatus::persistence_failed:
        return result;
    case OperationReconciliationStatus::confirmed:
        if (!cycle.record ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record) ||
            cycle.record->settled_volume != state_.slice.result_volume)
            break;
        {
            const auto cumulative = history_deal_cumulative_volume(
                cycle, worker, *cycle.record, DealDirection::entry);
            if (!cumulative || *cumulative == 0 ||
                *cumulative < cycle.record->settled_volume ||
                *cumulative > state_.slice.requested_volume)
                break;

            const auto delta = *cumulative - cycle.record->settled_volume;
            if (delta == 0 || delta > state_.pending_remainder_volume)
                break;
            auto candidate = state_;
            if (candidate.observe_pending_remainder(delta) !=
                managed_trade::MutationStatus::applied)
                break;
            const auto target_state = candidate.slice.state ==
                                              managed_trade::OperationState::filled
                                          ? OperationState::filled
                                          : OperationState::partially_filled;
            const auto transitioned =
                journal_.transition_operation(*key, target_state, *cumulative);
            if (!transitioned.accepted())
                break;
            state_ = std::move(candidate);
            result.record = transitioned.record;
            result.status = target_state == OperationState::filled
                                ? OwnerStepStatus::completed
                                : OwnerStepStatus::partially_filled;
            return result;
        }
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    }

    if (cycle.status == OperationReconciliationStatus::confirmed)
        result.status = OwnerStepStatus::awaiting_reconciliation;
    return result;
}

OwnerStepResult ManagedTradeOwner::settle_close_reconciliation(
    OperationReconciliationWorker &worker, bool trade_event_gap,
    bool deadline_expired) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.state != managed_trade::OperationState::submitting ||
        state_.slice.kind != managed_trade::OperationKind::close)
        return result_for(OwnerStepStatus::invalid_state, current_key());

    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    if (worker.key() != *key)
        return result_for(OwnerStepStatus::invalid_request, key,
                          journal_.find(*key));
    const auto current = journal_.find(*key);
    if (!current || current->operation_kind != OperationKind::close ||
        !current->reconciliation_descriptor)
        return result_for(OwnerStepStatus::invalid_state, key, current);
    const auto settled_state = current->reconciliation_descriptor->settled_state;
    if (settled_state != OperationState::filled &&
        settled_state != OperationState::partially_filled)
        return result_for(OwnerStepStatus::invalid_state, key, current);

    const auto cycle = worker.step(trade_event_gap, deadline_expired);
    auto result = result_for(OwnerStepStatus::durable_failure, key, cycle.record);
    if (!result.record)
        result.record = journal_.find(*key);

    switch (cycle.status) {
    case OperationReconciliationStatus::pending:
    case OperationReconciliationStatus::not_observed:
    case OperationReconciliationStatus::trade_event_gap:
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    case OperationReconciliationStatus::account_mismatch:
    case OperationReconciliationStatus::ambiguous:
        if (!cycle.record ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record)) {
            result.status = OwnerStepStatus::ambiguous;
            return result;
        }
        if (cycle.record->operation_state != OperationState::ambiguous)
            return result;
        {
            auto candidate = state_;
            if (candidate.record_unknown() != managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                return result;
            state_ = std::move(candidate);
        }
        result.status = OwnerStepStatus::ambiguous;
        return result;
    case OperationReconciliationStatus::invalid_request:
        result.status = OwnerStepStatus::invalid_request;
        return result;
    case OperationReconciliationStatus::operation_not_found:
    case OperationReconciliationStatus::invalid_state:
        result.status = OwnerStepStatus::invalid_state;
        return result;
    case OperationReconciliationStatus::persistence_failed:
        return result;
    case OperationReconciliationStatus::confirmed:
        if (!cycle.record || cycle.record->operation_kind != OperationKind::close ||
            cycle.record->operation_state != OperationState::reconciling ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record))
            break;
        {
            const auto executed = history_deal_volume(
                cycle, worker, *cycle.record, DealDirection::exit);
            if (!executed || *executed == 0 ||
                *executed > state_.slice.requested_volume ||
                *executed > state_.open_volume)
                break;
            const auto target_state =
                *executed == state_.slice.requested_volume
                    ? OperationState::filled
                    : OperationState::partially_filled;
            auto candidate = state_;
            if (candidate.record_fill(*executed) !=
                    managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                break;
            const auto transitioned =
                journal_.transition_operation(*key, target_state, *executed);
            if (!transitioned.accepted())
                break;
            state_ = std::move(candidate);
            result.record = transitioned.record;
            result.status = target_state == OperationState::filled
                                 ? OwnerStepStatus::completed
                                 : OwnerStepStatus::partially_filled;
            return result;
        }
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    }

    if (cycle.status == OperationReconciliationStatus::confirmed)
        result.status = OwnerStepStatus::awaiting_reconciliation;
    return result;
}

OwnerStepResult ManagedTradeOwner::settle_cancel_reconciliation(
    OperationReconciliationWorker &worker, bool trade_event_gap,
    bool deadline_expired) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.state != managed_trade::OperationState::submitting ||
        state_.slice.kind != managed_trade::OperationKind::cancel)
        return result_for(OwnerStepStatus::invalid_state, current_key());

    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    if (worker.key() != *key)
        return result_for(OwnerStepStatus::invalid_request, key,
                          journal_.find(*key));
    const auto current = journal_.find(*key);
    if (!current || current->operation_kind != OperationKind::cancel ||
        !current->reconciliation_descriptor ||
        current->reconciliation_descriptor->settled_state != OperationState::cancelled)
        return result_for(OwnerStepStatus::invalid_state, key, current);

    const auto cycle = worker.step(trade_event_gap, deadline_expired);
    auto result = result_for(OwnerStepStatus::durable_failure, key, cycle.record);
    if (!result.record)
        result.record = journal_.find(*key);

    switch (cycle.status) {
    case OperationReconciliationStatus::pending:
    case OperationReconciliationStatus::not_observed:
    case OperationReconciliationStatus::trade_event_gap:
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    case OperationReconciliationStatus::account_mismatch:
    case OperationReconciliationStatus::ambiguous:
        if (!cycle.record ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record)) {
            result.status = OwnerStepStatus::ambiguous;
            return result;
        }
        if (cycle.record->operation_state != OperationState::ambiguous)
            return result;
        {
            auto candidate = state_;
            if (candidate.record_unknown() != managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                return result;
            state_ = std::move(candidate);
        }
        result.status = OwnerStepStatus::ambiguous;
        return result;
    case OperationReconciliationStatus::invalid_request:
        result.status = OwnerStepStatus::invalid_request;
        return result;
    case OperationReconciliationStatus::operation_not_found:
    case OperationReconciliationStatus::invalid_state:
        result.status = OwnerStepStatus::invalid_state;
        return result;
    case OperationReconciliationStatus::persistence_failed:
        return result;
    case OperationReconciliationStatus::confirmed:
        if (!cycle.record || cycle.record->operation_kind != OperationKind::cancel ||
            cycle.record->operation_state != OperationState::reconciling ||
            !has_observation_provenance(cycle, worker, *key, *cycle.record))
            break;
        {
            auto candidate = state_;
            if (candidate.record_cancel_accepted() !=
                    managed_trade::MutationStatus::applied ||
                candidate.reconcile() != managed_trade::MutationStatus::applied)
                break;
            const auto transitioned = journal_.transition_operation(
                *key, OperationState::cancelled);
            if (!transitioned.accepted())
                break;
            state_ = std::move(candidate);
            result.record = transitioned.record;
            result.status = OwnerStepStatus::completed;
            return result;
        }
        result.status = OwnerStepStatus::awaiting_reconciliation;
        return result;
    }

    if (cycle.status == OperationReconciliationStatus::confirmed)
        result.status = OwnerStepStatus::awaiting_reconciliation;
    return result;
}

bool ManagedTradeOwner::mark_journal_reconciling(const OperationKey &key) {
    auto record = journal_.find(key);
    if (!record || !journal_at_least_dispatching(record->journal_state))
        return false;

    if (record->journal_state != JournalState::reconciling) {
        const auto journal_state = journal_.transition_journal(
            key, JournalState::reconciling);
        if (!journal_state.accepted())
            return false;
        record = journal_state.record;
    }
    return record.has_value();
}

bool ManagedTradeOwner::state_to_submitting(
    managed_trade::ManagedTradeState *candidate) const {
    return candidate &&
           (candidate->slice.state == managed_trade::OperationState::submitting ||
            candidate->enter_submitting() == managed_trade::MutationStatus::applied);
}

bool ManagedTradeOwner::has_observation_provenance(
    const OperationReconciliationCycle &cycle,
    const OperationReconciliationWorker &worker, const OperationKey &key,
    const OperationRecord &record) const {
    if (!record.valid() || record.key != key || !cycle.observation ||
        !cycle.observation->refresh.apply.accepted() ||
        !cycle.observation->refresh.sample)
        return false;

    const auto &sample = *cycle.observation->refresh.sample;
    if (sample.batch().account != key.account || sample.graph_instance_id() == 0 ||
        sample.graph_revision() == 0 ||
        sample.graph_instance_id() != worker.graph().instance_id() ||
        sample.graph_revision() != worker.graph().revision() ||
        sample.graph_revision() != cycle.observation->reconciliation.evaluated_revision)
        return false;
    const auto &baseline = worker.effective_baseline();
    if (!baseline || baseline->account() != key.account ||
        baseline->graph_instance_id() != sample.graph_instance_id() ||
        sample.graph_revision() <= baseline->graph_revision())
        return false;

    const auto durable = journal_.find(key);
    return durable && durable->key == record.key &&
           durable->revision == record.revision &&
           durable->journal_state == record.journal_state &&
           durable->operation_state == record.operation_state;
}

std::optional<ManagedTradeOwner::HistoryDealAttributionContext>
ManagedTradeOwner::history_deal_attribution_context(
    const OperationReconciliationCycle &cycle,
    const OperationReconciliationWorker &worker,
    const OperationRecord &record, DealDirection direction) const {
    if (!cycle.observation || !cycle.observation->refresh.sample ||
        !record.reconciliation_descriptor)
        return std::nullopt;

    const auto &sample = *cycle.observation->refresh.sample;
    if (!observes(sample.batch().observed_domains,
                  ObservationDomain::history_deals) ||
        !sample.batch().history_deals_window)
        return std::nullopt;

    const auto &baseline = worker.effective_baseline();
    if (!baseline || !baseline->valid() ||
        worker.graph().domain_revision(ObservationDomain::history_deals) <=
            baseline->history_deals_revision())
        return std::nullopt;

    std::optional<ObservationWindow> requested_window;
    std::vector<std::uint64_t> anchor_tickets;
    std::size_t history_deal_predicate_count = 0;
    constexpr std::uint32_t kDealEntryIn = 0;
    constexpr std::uint32_t kDealEntryOut = 1;
    constexpr std::uint32_t kDealEntryInOut = 2;
    constexpr std::uint32_t kDealEntryOutBy = 3;
    constexpr std::uint64_t kRequiredDealFields =
        MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
        MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
        MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME;
    for (const auto &predicate : record.reconciliation_descriptor->predicates) {
        if (predicate.kind != ReconciliationPredicateKind::history_deal_present)
            continue;
        ++history_deal_predicate_count;
        if (history_deal_predicate_count != 1 || predicate.ticket != 0 ||
            predicate.correlation_id == 0)
            return std::nullopt;
        if (!predicate.history_window)
            return std::nullopt;
        if (!requested_window)
            requested_window = predicate.history_window;
        else if (requested_window->from_msc != predicate.history_window->from_msc ||
                 requested_window->to_msc != predicate.history_window->to_msc)
            return std::nullopt;

        std::size_t matching_bindings = 0;
        std::uint64_t ticket = 0;
        for (const auto &binding : record.reconciliation_bindings) {
            if (binding.correlation_id != predicate.correlation_id)
                continue;
            ++matching_bindings;
            if (!binding.valid() || ticket != 0)
                return std::nullopt;
            ticket = binding.broker_ticket;
        }
        if (matching_bindings != 1 || ticket == 0)
            return std::nullopt;
        anchor_tickets.push_back(ticket);
    }
    if (!requested_window || anchor_tickets.empty() ||
        sample.batch().history_deals_window->from_msc > requested_window->from_msc ||
        sample.batch().history_deals_window->to_msc < requested_window->to_msc ||
        !worker.graph().history_deals_covered(
            *requested_window, baseline->history_deals_revision()))
        return std::nullopt;

    std::uint64_t order_ticket = 0;
    std::uint64_t position_id = 0;
    bool order_selected = false;
    const auto deals = worker.graph().history_deals();
    for (const auto anchor : anchor_tickets) {
        const auto found = std::find_if(
            deals.begin(), deals.end(), [anchor](const Mt5DealSnapshot &deal) {
                return deal.ticket == anchor;
            });
        const bool valid_entry = found != deals.end() &&
                                 (found->entry == kDealEntryIn ||
                                  found->entry == kDealEntryInOut);
        const bool valid_exit = found != deals.end() &&
                                (found->entry == kDealEntryOut ||
                                 found->entry == kDealEntryOutBy);
        if (found == deals.end() ||
            worker.graph().history_deal_evidence_revision(found->ticket) <=
                baseline->history_deals_revision() ||
            (found->known_fields & kRequiredDealFields) != kRequiredDealFields ||
            found->order_ticket == 0 ||
            found->position_id == 0 ||
            found->time_msc < requested_window->from_msc ||
            found->time_msc > requested_window->to_msc ||
            (direction == DealDirection::entry ? !valid_entry : !valid_exit))
            return std::nullopt;
        if (!order_selected) {
            order_ticket = found->order_ticket;
            position_id = found->position_id;
            order_selected = true;
        } else if (order_ticket != found->order_ticket ||
                   position_id != found->position_id) {
            return std::nullopt;
        }
    }

    return HistoryDealAttributionContext{*requested_window, order_ticket,
                                         position_id};
}

std::optional<managed_trade::Volume> ManagedTradeOwner::history_deal_volume(
    const OperationReconciliationCycle &cycle,
    const OperationReconciliationWorker &worker,
    const OperationRecord &record, DealDirection direction) const {
    const auto context =
        history_deal_attribution_context(cycle, worker, record, direction);
    if (!context)
        return std::nullopt;

    const auto &baseline = worker.effective_baseline();
    if (!baseline)
        return std::nullopt;
    constexpr std::uint32_t kDealEntryIn = 0;
    constexpr std::uint32_t kDealEntryOut = 1;
    constexpr std::uint32_t kDealEntryInOut = 2;
    constexpr std::uint32_t kDealEntryOutBy = 3;
    constexpr std::uint64_t kRequiredDealFields =
        MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
        MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
        MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME;

    long double total = 0.0L;
    bool attributed_entry = false;
    for (const auto &deal : worker.graph().history_deals()) {
        if (deal.order_ticket != context->order_ticket ||
            deal.position_id != context->position_id ||
            worker.graph().history_deal_evidence_revision(deal.ticket) <=
                baseline->history_deals_revision() ||
            deal.time_msc < context->requested_window.from_msc ||
            deal.time_msc > context->requested_window.to_msc)
            continue;
        if ((deal.known_fields & kRequiredDealFields) != kRequiredDealFields)
            return std::nullopt;
        const bool entry_deal = deal.entry == kDealEntryIn ||
                                deal.entry == kDealEntryInOut;
        const bool exit_deal = deal.entry == kDealEntryOut ||
                               deal.entry == kDealEntryOutBy;
        if (direction == DealDirection::entry ? !entry_deal : !exit_deal)
            continue;
        if (!std::isfinite(deal.volume) || deal.volume <= 0.0)
            return std::nullopt;
        total += static_cast<long double>(deal.volume);
        if (!std::isfinite(total))
            return std::nullopt;
        attributed_entry = true;
    }
    if (!attributed_entry || total <= 0.0L ||
        std::floor(total) != total ||
        total > static_cast<long double>((std::numeric_limits<managed_trade::Volume>::max)()))
        return std::nullopt;
    return static_cast<managed_trade::Volume>(total);
}

std::optional<managed_trade::Volume>
ManagedTradeOwner::history_deal_cumulative_volume(
    const OperationReconciliationCycle &cycle,
    const OperationReconciliationWorker &worker,
    const OperationRecord &record, DealDirection direction) const {
    const auto context =
        history_deal_attribution_context(cycle, worker, record, direction);
    if (!context)
        return std::nullopt;

    constexpr std::uint32_t kDealEntryIn = 0;
    constexpr std::uint32_t kDealEntryOut = 1;
    constexpr std::uint32_t kDealEntryInOut = 2;
    constexpr std::uint32_t kDealEntryOutBy = 3;
    constexpr std::uint64_t kRequiredDealFields =
        MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
        MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
        MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME;

    long double total = 0.0L;
    bool attributed_entry = false;
    for (const auto &deal : worker.graph().history_deals()) {
        if (deal.order_ticket != context->order_ticket ||
            deal.position_id != context->position_id ||
            deal.time_msc < context->requested_window.from_msc ||
            deal.time_msc > context->requested_window.to_msc)
            continue;
        if ((deal.known_fields & kRequiredDealFields) != kRequiredDealFields)
            return std::nullopt;
        const bool entry_deal = deal.entry == kDealEntryIn ||
                                deal.entry == kDealEntryInOut;
        const bool exit_deal = deal.entry == kDealEntryOut ||
                               deal.entry == kDealEntryOutBy;
        if (direction == DealDirection::entry ? !entry_deal : !exit_deal)
            continue;
        if (!std::isfinite(deal.volume) || deal.volume <= 0.0)
            return std::nullopt;
        total += static_cast<long double>(deal.volume);
        if (!std::isfinite(total))
            return std::nullopt;
        attributed_entry = true;
    }
    if (!attributed_entry || total <= 0.0L || std::floor(total) != total ||
        total > static_cast<long double>((std::numeric_limits<managed_trade::Volume>::max)()))
        return std::nullopt;
    return static_cast<managed_trade::Volume>(total);
}

} // namespace mt5bridge::dispatch

/// \file dispatch/managed_trade_owner.cpp
/// \brief Implements the private managed-trade owner-loop bridge.

#include "managed_trade_owner.hpp"

#include <utility>

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

OwnerStepResult ManagedTradeOwner::prepare_open(managed_trade::Volume volume,
                                                ManagedTradeIntent intent) {
    if (!account_.valid() || !state_.valid() || intent.request_payload.empty() ||
        !intent.reconciliation_descriptor.valid() ||
        intent.reconciliation_descriptor.account != account_)
        return result_for(OwnerStepStatus::invalid_request);

    auto candidate = state_;
    if (candidate.start_open_slice(volume) != managed_trade::MutationStatus::applied)
        return result_for(OwnerStepStatus::invalid_state);

    const OperationKey key{account_, candidate.trade_id.value,
                           candidate.slice.operation_id};
    if (journal_.find(key))
        return result_for(OwnerStepStatus::invalid_state, key, journal_.find(key));

    const auto created = journal_.create(key, std::move(intent.request_payload));
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
        settled_state != OperationState::rejected)
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
    case OperationReconciliationStatus::progressed:
        break;
    case OperationReconciliationStatus::invalid_request:
        result.status = OwnerStepStatus::invalid_request;
        return result;
    case OperationReconciliationStatus::operation_not_found:
    case OperationReconciliationStatus::invalid_state:
        result.status = OwnerStepStatus::invalid_state;
        return result;
    case OperationReconciliationStatus::persistence_failed:
        return result;
    }

    if (!cycle.record || !has_observation_provenance(cycle, worker, *key,
                                                      *cycle.record))
        return result;
    if (cycle.record->operation_state != settled_state)
        return result;

    auto candidate = state_;
    const auto recorded = settled_state == OperationState::rejected
                              ? candidate.record_rejected()
                              : candidate.record_fill(candidate.slice.requested_volume);
    if (recorded != managed_trade::MutationStatus::applied ||
        candidate.reconcile() != managed_trade::MutationStatus::applied)
        return result;
    state_ = std::move(candidate);
    result.status = OwnerStepStatus::completed;
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
    if (!record.reconciliation_descriptor ||
        sample.graph_revision() <=
            record.reconciliation_descriptor->baseline.graph_revision())
        return false;

    const auto durable = journal_.find(key);
    return durable && durable->key == record.key &&
           durable->revision == record.revision &&
           durable->journal_state == record.journal_state &&
           durable->operation_state == record.operation_state;
}

} // namespace mt5bridge::dispatch

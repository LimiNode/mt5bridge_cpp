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

std::optional<mt5bridge::OperationState> journal_state_for(
    managed_trade::OperationState state) {
    switch (state) {
    case managed_trade::OperationState::partially_filled:
        return mt5bridge::OperationState::partially_filled;
    case managed_trade::OperationState::filled:
        return mt5bridge::OperationState::filled;
    case managed_trade::OperationState::cancelled:
        return mt5bridge::OperationState::cancelled;
    case managed_trade::OperationState::rejected:
        return mt5bridge::OperationState::rejected;
    case managed_trade::OperationState::ambiguous:
        return mt5bridge::OperationState::ambiguous;
    default:
        return std::nullopt;
    }
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
            result.status = OwnerStepStatus::awaiting_evidence;
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
        result.status = OwnerStepStatus::awaiting_evidence;
        return result;
    }

    if (uncertain_after_barrier) {
        if (!mark_journal_ambiguous(*key)) {
            result.status = OwnerStepStatus::durable_failure;
            result.record = journal_.find(*key);
            return result;
        }
        auto candidate = state_;
        if (!state_to_submitting(&candidate) ||
            candidate.record_unknown() != managed_trade::MutationStatus::applied ||
            candidate.reconcile() != managed_trade::MutationStatus::applied)
            return result;
        state_ = std::move(candidate);
        result.status = OwnerStepStatus::ambiguous;
        result.record = journal_.find(*key);
    }
    return result;
}

OwnerStepResult ManagedTradeOwner::apply_evidence(BrokerEvidence evidence) {
    if (!state_.valid())
        return result_for(OwnerStepStatus::invalid_request);
    if (state_.slice.state != managed_trade::OperationState::submitting)
        return result_for(OwnerStepStatus::invalid_state, current_key());
    if ((evidence.kind == BrokerEvidenceKind::fill && evidence.volume == 0) ||
        (evidence.kind != BrokerEvidenceKind::fill && evidence.volume != 0))
        return result_for(OwnerStepStatus::invalid_request, current_key());
    const auto key = current_key();
    if (!key)
        return result_for(OwnerStepStatus::invalid_request);
    const auto record = journal_.find(*key);
    if (!record || !is_result_pending(*record) || record->result_payload.empty())
        return result_for(OwnerStepStatus::invalid_state, key, record);

    auto candidate = state_;
    if (!apply_evidence_to_state(&candidate, evidence))
        return result_for(OwnerStepStatus::invalid_state, key, record);
    const auto final_state = journal_state_for(candidate.slice.state);
    if (!final_state)
        return result_for(OwnerStepStatus::invalid_state, key, record);

    auto current = record;
    if (current->operation_state == OperationState::accepted) {
        const auto reconciling = journal_.transition_operation(
            *key, OperationState::reconciling);
        if (!reconciling.accepted())
            return result_for(OwnerStepStatus::durable_failure, key,
                              reconciling.record);
        current = reconciling.record;
    }
    if (!current || current->journal_state != JournalState::reconciling) {
        const auto journal_state = journal_.transition_journal(
            *key, JournalState::reconciling);
        if (!journal_state.accepted())
            return result_for(OwnerStepStatus::durable_failure, key,
                              journal_state.record);
        current = journal_state.record;
    }
    const auto advanced = journal_.transition_operation(*key, *final_state);
    if (!advanced.accepted())
        return result_for(OwnerStepStatus::durable_failure, key, advanced.record);

    const auto status = candidate.slice.state == managed_trade::OperationState::ambiguous
                            ? OwnerStepStatus::ambiguous
                            : OwnerStepStatus::completed;
    state_ = std::move(candidate);
    return result_for(status, key, advanced.record);
}

bool ManagedTradeOwner::mark_journal_ambiguous(const OperationKey &key) {
    auto record = journal_.find(key);
    if (!record || !journal_at_least_dispatching(record->journal_state))
        return false;

    if (record->operation_state != OperationState::reconciling &&
        record->operation_state != OperationState::ambiguous) {
        const auto reconciling = journal_.transition_operation(
            key, OperationState::reconciling);
        if (!reconciling.accepted())
            return false;
        record = reconciling.record;
    }
    if (!record || record->journal_state != JournalState::reconciling) {
        const auto journal_state = journal_.transition_journal(
            key, JournalState::reconciling);
        if (!journal_state.accepted())
            return false;
        record = journal_state.record;
    }
    if (!record || record->operation_state == OperationState::ambiguous)
        return record.has_value();
    return journal_.transition_operation(key, OperationState::ambiguous).accepted();
}

bool ManagedTradeOwner::state_to_submitting(
    managed_trade::ManagedTradeState *candidate) const {
    return candidate &&
           (candidate->slice.state == managed_trade::OperationState::submitting ||
            candidate->enter_submitting() == managed_trade::MutationStatus::applied);
}

bool ManagedTradeOwner::apply_evidence_to_state(
    managed_trade::ManagedTradeState *candidate, BrokerEvidence evidence) const {
    if (!candidate)
        return false;
    managed_trade::MutationStatus status = managed_trade::MutationStatus::invalid_state;
    switch (evidence.kind) {
    case BrokerEvidenceKind::fill:
        status = candidate->record_fill(evidence.volume);
        break;
    case BrokerEvidenceKind::cancel_accepted:
        status = candidate->record_cancel_accepted();
        break;
    case BrokerEvidenceKind::rejected:
        status = candidate->record_rejected();
        break;
    case BrokerEvidenceKind::unknown:
        status = candidate->record_unknown();
        break;
    }
    return status == managed_trade::MutationStatus::applied &&
           candidate->reconcile() == managed_trade::MutationStatus::applied;
}

} // namespace mt5bridge::dispatch

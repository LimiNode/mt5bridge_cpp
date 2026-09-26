/// \file one_shot_backend.cpp
/// \brief Implements guarded one-shot dispatch execution.

#include "one_shot_backend.hpp"

#include <limits>
#include <utility>

namespace mt5bridge::runtime {

OneShotExecutionResult OneShotDispatchBackend::execute(
    OperationJournal &journal, const OperationKey &key, DispatchPermit permit,
    CurrentAccountProbe &account_probe, const SingleWriterLease &lease,
    DispatchTransport &transport) const {
    if (!permit.valid() || !key.valid() || permit.key() != key)
        return {OneShotExecutionStatus::invalid_permit, 0, std::nullopt};
    std::optional<AccountKey> account_before;
    try {
        account_before = account_probe.current_account();
    } catch (...) {
        return {OneShotExecutionStatus::account_mismatch, 0, std::nullopt};
    }
    if (!account_before || !account_before->valid() || *account_before != key.account)
        return {OneShotExecutionStatus::account_mismatch, 0, std::nullopt};

    const auto record = journal.find(key);
    if (!record)
        return {OneShotExecutionStatus::operation_not_found, 0, std::nullopt};
    if (record->journal_state != JournalState::dispatching ||
        record->operation_state != OperationState::prechecking ||
        record->revision != permit.journal_revision() ||
        record->fencing_token != permit.fencing_token())
        return {OneShotExecutionStatus::stale_permit, 0, record};

    const auto initial_token = lease.held_fencing_token(key.account);
    if (!initial_token || *initial_token != permit.fencing_token())
        return {OneShotExecutionStatus::lease_lost, 0, record};

    const auto submitting =
        journal.transition_operation(key, OperationState::submitting);
    if (!submitting.accepted())
        return {OneShotExecutionStatus::transition_failed, 0, journal.find(key)};

    const auto before_call = journal.find(key);
    if (!before_call || before_call->journal_state != JournalState::dispatching ||
        before_call->operation_state != OperationState::submitting ||
        before_call->fencing_token != permit.fencing_token() ||
        permit.journal_revision() == (std::numeric_limits<std::uint64_t>::max)() ||
        before_call->revision != permit.journal_revision() + 1)
        return {OneShotExecutionStatus::stale_permit, 0, before_call};
    std::optional<AccountKey> account_before_call;
    try {
        account_before_call = account_probe.current_account();
    } catch (...) {
        return {OneShotExecutionStatus::account_mismatch, 0, before_call};
    }
    if (!account_before_call || !account_before_call->valid() ||
        *account_before_call != key.account)
        return {OneShotExecutionStatus::account_mismatch, 0, before_call};

    const auto call_token = lease.held_fencing_token(key.account);
    if (!call_token || *call_token != permit.fencing_token())
        return {OneShotExecutionStatus::lease_lost, 0, before_call};

    BackendCallResult call;
    try {
        call = transport.submit_once(*before_call);
    } catch (...) {
        return {OneShotExecutionStatus::transport_failure, 0, journal.find(key)};
    }
    if (call.status == BackendCallStatus::account_mismatch)
        return {OneShotExecutionStatus::account_mismatch, 0, journal.find(key)};
    if (call.status == BackendCallStatus::transport_failure)
        return {OneShotExecutionStatus::transport_failure, 0, journal.find(key)};
    if (call.raw_result.empty())
        return {OneShotExecutionStatus::result_not_durable, call.retcode,
                journal.find(key)};

    const bool has_bindings = !call.reconciliation_bindings.empty();
    const auto persisted = journal.persist_result_with_bindings(
        key, std::move(call.raw_result), std::move(call.reconciliation_bindings));
    if (!persisted.accepted())
        return {has_bindings ? OneShotExecutionStatus::reconciliation_binding_failed
                             : OneShotExecutionStatus::result_not_durable,
                call.retcode, journal.find(key)};

    OperationState final_state = OperationState::reconciling;
    if (call.retcode == kTradeRetcodeMarketClosed ||
        call.disposition == BrokerResultDisposition::rejected)
        final_state = OperationState::rejected;
    else if (call.disposition == BrokerResultDisposition::accepted)
        final_state = OperationState::accepted;

    const auto advanced = journal.transition_operation(key, final_state);
    if (!advanced.accepted())
        return {OneShotExecutionStatus::lifecycle_transition_failed, call.retcode,
                journal.find(key)};
    return {OneShotExecutionStatus::completed, call.retcode, advanced.record};
}

} // namespace mt5bridge::runtime

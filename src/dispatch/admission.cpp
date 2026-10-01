/// \file dispatch/admission.cpp
/// \brief Implements the durable pre-side-effect dispatch admission barrier.

#include <mt5bridge/dispatch/admission.hpp>
#include <mt5bridge/dispatch/journal.hpp>

namespace mt5bridge {

DispatchAdmissionBarrier::DispatchAdmissionBarrier(
    OperationJournal &journal, const ObservationGraph &graph,
    EnvironmentConsistencyRequest required_scope)
    : journal_(journal), graph_(graph), required_scope_(std::move(required_scope)) {}

DispatchAdmissionResult DispatchAdmissionBarrier::admit(
    const OperationKey &key, const DispatchAdmissionRequest &request,
    const SingleWriterLease &lease) {
    if (!required_scope_.valid())
        return {DispatchAdmissionStatus::invalid_request, std::nullopt};
    if (!key.valid() || !request.current_account.valid())
        return {DispatchAdmissionStatus::invalid_request, std::nullopt};
    if (request.unresolved_operation)
        return {DispatchAdmissionStatus::unresolved_operation, std::nullopt};
    if (request.event_gap)
        return {DispatchAdmissionStatus::event_gap, std::nullopt};
    if (request.current_account != key.account)
        return {DispatchAdmissionStatus::account_mismatch, std::nullopt};
    const auto record = journal_.find(key);
    if (!record)
        return {DispatchAdmissionStatus::operation_not_found, std::nullopt};
    if (record->journal_state != JournalState::dispatch_intent_persisted ||
        record->operation_state != OperationState::prechecking ||
        !record->reconciliation_descriptor)
        return {DispatchAdmissionStatus::invalid_state, std::nullopt};
    if (!reconciliation_descriptor_matches_graph(*record->reconciliation_descriptor,
                                                  graph_))
        return {DispatchAdmissionStatus::invalid_state, std::nullopt};
    const auto effective_scope = effective_dispatch_scope(
        *record->reconciliation_descriptor, required_scope_);
    if (!effective_scope)
        return {DispatchAdmissionStatus::invalid_state, std::nullopt};
    if (!request.environment_proof || !request.environment_proof->valid())
        return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};
    if (request.environment_proof->account() != key.account)
        return {DispatchAdmissionStatus::account_mismatch, std::nullopt};
    if (!request.environment_proof->covers(*effective_scope))
        return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};
    if (request.environment_proof->graph_instance_id() != graph_.instance_id() ||
        request.environment_proof->last_graph_revision() != graph_.revision())
        return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};

    const auto fencing_token = lease.held_fencing_token(key.account);
    if (!fencing_token || *fencing_token == 0)
        return {DispatchAdmissionStatus::lease_not_held, std::nullopt};

    const auto committed = journal_.transition_journal(
        key, JournalState::dispatching, *fencing_token);
    if (!committed.accepted())
        return {committed.status == JournalMutationStatus::conflict
                    ? DispatchAdmissionStatus::conflict
                    : committed.status == JournalMutationStatus::not_durable
                          ? DispatchAdmissionStatus::not_durable
                          : DispatchAdmissionStatus::invalid_state,
                std::nullopt};
    return {DispatchAdmissionStatus::admitted,
            std::optional<DispatchPermit>(DispatchPermit::create(
                key, committed.record->fencing_token, committed.record->revision))};
}

} // namespace mt5bridge

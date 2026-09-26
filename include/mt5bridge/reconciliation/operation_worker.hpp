#pragma once

/// \file reconciliation/operation_worker.hpp
/// \brief Defines journal-aware operation reconciliation and startup recovery.

#include <mt5bridge/dispatch/journal.hpp>
#include <mt5bridge/observation/worker.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge {

/// \enum OperationRecoveryAction
/// \brief Describes the safe owner-loop action for a recovered record.
enum class OperationRecoveryAction {
    resume_pre_dispatch, ///< Continue validation/admission without a send.
    reconcile_only,      ///< The non-resendable barrier was crossed.
    terminal,            ///< The durable lifecycle is already terminal.
};

/// \struct RecoveredOperation
/// \brief Couples a recovered durable record with its fail-closed action.
struct RecoveredOperation {
    OperationRecord record; ///< Complete validated durable record.
    OperationRecoveryAction action = OperationRecoveryAction::terminal;

    /// \brief Tests whether recovery must use observation only.
    /// \return True after the dispatch barrier was crossed.
    bool non_resendable() const {
        return action == OperationRecoveryAction::reconcile_only;
    }
};

/// \struct OperationRecoveryResult
/// \brief Returns all recovered operations or an all-or-nothing failure.
struct OperationRecoveryResult {
    JournalMutationStatus status = JournalMutationStatus::invalid_record;
    std::vector<RecoveredOperation> operations;

    /// \brief Tests whether the complete durable set was classified.
    /// \return True only when recovery and classification both succeeded.
    bool accepted() const { return status == JournalMutationStatus::accepted; }
};

/// \class OperationRecoveryCoordinator
/// \brief Classifies every durable operation after an owner-loop restart.
///
/// This coordinator never resends and never mutates recovered records. Records
/// at or beyond `dispatching` are explicitly reconciliation-only, including a
/// crash between durable `submitting` and the backend call.
class OperationRecoveryCoordinator {
public:
    /// \brief Recovers and classifies the complete journal atomically.
    /// \param journal Journal whose owner cache is replaced on success.
    /// \return Complete classification, or a failure with no partial result.
    static OperationRecoveryResult recover(OperationJournal &journal) {
        const auto recovered = journal.recover_all();
        if (!recovered.accepted())
            return {recovered.status, {}};

        OperationRecoveryResult result;
        result.status = JournalMutationStatus::accepted;
        result.operations.reserve(recovered.records.size());
        for (const auto &record : recovered.records) {
            if (!record.valid())
                return {JournalMutationStatus::invalid_record, {}};
            result.operations.push_back({record, classify(record)});
        }
        return result;
    }

    /// \brief Classifies one already validated operation record.
    /// \param record Durable operation record.
    /// \return Safe owner-loop action for the record.
    static OperationRecoveryAction classify(const OperationRecord &record) {
        if (record.operation_state == OperationState::filled ||
            record.operation_state == OperationState::cancelled ||
            record.operation_state == OperationState::expired ||
            record.operation_state == OperationState::rejected ||
            record.operation_state == OperationState::failed ||
            record.operation_state == OperationState::ambiguous)
            return OperationRecoveryAction::terminal;
        if (journal_at_least_dispatching(record.journal_state))
            return OperationRecoveryAction::reconcile_only;
        switch (record.operation_state) {
        case OperationState::queued:
        case OperationState::prechecking:
            return OperationRecoveryAction::resume_pre_dispatch;
        case OperationState::submitting:
        case OperationState::accepted:
        case OperationState::reconciling:
        case OperationState::partially_filled:
        case OperationState::filled:
        case OperationState::cancelled:
        case OperationState::expired:
        case OperationState::rejected:
        case OperationState::failed:
        case OperationState::ambiguous:
            return OperationRecoveryAction::terminal;
        }
        return OperationRecoveryAction::terminal;
    }
};

/// \enum OperationReconciliationStatus
/// \brief Reports one journal-aware reconciliation cycle.
enum class OperationReconciliationStatus {
    progressed,          ///< Evidence settled and lifecycle was durably advanced.
    pending,             ///< More authoritative observations are required.
    not_observed,        ///< Deadline elapsed without proof; operation remains open.
    trade_event_gap,     ///< Event continuity was lost; operation remains open.
    account_mismatch,    ///< Account changed; no lifecycle guess was persisted.
    ambiguous,            ///< Evidence is contradictory or cannot be attributed.
    invalid_request,      ///< The supplied predicates or settlement state are invalid.
    operation_not_found,  ///< The journal has no owner-cache record for the key.
    invalid_state,        ///< The record is not eligible for reconciliation.
    persistence_failed,  ///< A required durable transition failed.
};

/// \struct OperationReconciliationCycle
/// \brief Couples observation evidence with its durable lifecycle outcome.
struct OperationReconciliationCycle {
    OperationReconciliationStatus status =
        OperationReconciliationStatus::operation_not_found;
    std::optional<ReconciliationWorkerCycle> observation;
    std::optional<OperationRecord> record;

    /// \brief Tests whether this cycle durably settled the operation.
    /// \return True only after a successful lifecycle transition.
    bool progressed() const {
        return status == OperationReconciliationStatus::progressed;
    }
};

/// \class OperationReconciliationWorker
/// \brief Couples bounded observation cycles to one durable operation.
///
/// The worker normalizes any recovered post-dispatch record to the
/// `reconciling` journal state before collecting evidence. `pending`,
/// `not_observed`, and event gaps never become terminal and never permit a
/// resend. The caller supplies explicit predicates and the lifecycle state
/// that those predicates prove; broker result payloads remain hints only.
class OperationReconciliationWorker {
public:
    /// \brief Binds one operation to a caller-driven observation worker.
    /// \param journal Owner-loop journal containing the operation.
    /// \param key Immutable operation identity.
    /// \param coordinator Coordinator used for authoritative refreshes.
    /// \param collection_request Domains refreshed on each cycle.
    /// \param reconciliation_request Explicit evidence predicates and baseline.
    /// \param settled_state State proven when all predicates are confirmed.
    OperationReconciliationWorker(
        OperationJournal &journal, OperationKey key,
        ObservationCoordinator &coordinator,
        ObservationCollectionRequest collection_request,
        ReconciliationRequest reconciliation_request,
        OperationState settled_state = OperationState::filled)
        : journal_(journal),
          key_(std::move(key)),
          descriptor_(make_descriptor(key_, reconciliation_request, settled_state)),
          worker_(coordinator, std::move(collection_request),
                  rebase_request(std::move(reconciliation_request), coordinator.graph())),
          settled_state_(settled_state) {}

    /// \brief Binds a worker directly to the journal's durable descriptor.
    /// \param journal Owner-loop journal containing the operation.
    /// \param key Immutable operation identity.
    /// \param coordinator Coordinator used for authoritative refreshes.
    /// \param collection_request Domains refreshed on each cycle.
    /// \note A graph from a new process is re-anchored before the first refresh;
    ///       the durable descriptor itself remains unchanged.
    OperationReconciliationWorker(
        OperationJournal &journal, OperationKey key,
        ObservationCoordinator &coordinator,
        ObservationCollectionRequest collection_request)
        : journal_(journal),
          key_(std::move(key)),
          descriptor_(journal_descriptor(journal_, key_)),
          worker_(coordinator, std::move(collection_request),
                  request_from_journal(journal_, key_, coordinator.graph())),
          settled_state_(descriptor_ ? descriptor_->settled_state
                                     : OperationState::filled) {}

    OperationReconciliationWorker(const OperationReconciliationWorker &) = delete;
    OperationReconciliationWorker &operator=(const OperationReconciliationWorker &) =
        delete;

    /// \brief Performs one refresh and applies only proven durable transitions.
    /// \param trade_event_gap True when event hints lost continuity.
    /// \param deadline_expired True when the bounded wait elapsed.
    /// \return Evidence and the resulting durable lifecycle status.
    OperationReconciliationCycle step(bool trade_event_gap = false,
                                      bool deadline_expired = false) {
        const auto current = journal_.find(key_);
        if (!current)
            return {OperationReconciliationStatus::operation_not_found,
                    std::nullopt, std::nullopt};
        if (!current->reconciliation_descriptor || !descriptor_)
            return {OperationReconciliationStatus::invalid_state, std::nullopt,
                    current};
        if (!same_descriptor(*current->reconciliation_descriptor, *descriptor_,
                             current->reconciliation_bindings))
            return {OperationReconciliationStatus::invalid_request, std::nullopt,
                    current};
        if (!valid_settled_state(settled_state_) ||
            is_terminal(current->operation_state))
            return {OperationReconciliationStatus::invalid_state, std::nullopt,
                    current};
        if (!journal_at_least_dispatching(current->journal_state))
            return {OperationReconciliationStatus::invalid_state, std::nullopt,
                    current};

        const auto normalized = normalize(*current);
        if (!normalized.accepted())
            return {OperationReconciliationStatus::persistence_failed,
                    std::nullopt, journal_.find(key_)};

        const auto cycle = worker_.step(trade_event_gap, deadline_expired);
        if (cycle.reconciliation.outcome == ReconciliationOutcome::pending)
            return {OperationReconciliationStatus::pending, cycle, journal_.find(key_)};
        if (cycle.reconciliation.outcome == ReconciliationOutcome::not_observed)
            return {OperationReconciliationStatus::not_observed, cycle,
                    journal_.find(key_)};
        if (cycle.reconciliation.outcome == ReconciliationOutcome::trade_event_gap)
            return {OperationReconciliationStatus::trade_event_gap, cycle,
                    journal_.find(key_)};
        if (cycle.reconciliation.outcome == ReconciliationOutcome::account_mismatch)
            return {OperationReconciliationStatus::account_mismatch, cycle,
                    journal_.find(key_)};
        if (cycle.reconciliation.reason == ReconciliationReason::invalid_request)
            return {OperationReconciliationStatus::invalid_request, cycle,
                    journal_.find(key_)};
        if (cycle.reconciliation.outcome == ReconciliationOutcome::ambiguous) {
            const auto advanced = journal_.transition_operation(
                key_, OperationState::ambiguous);
            if (!advanced.accepted())
                return {OperationReconciliationStatus::persistence_failed, cycle,
                        journal_.find(key_)};
            return {OperationReconciliationStatus::ambiguous, cycle, advanced.record};
        }
        if (cycle.reconciliation.outcome != ReconciliationOutcome::confirmed)
            return {OperationReconciliationStatus::invalid_request, cycle,
                    journal_.find(key_)};

        const auto advanced = journal_.transition_operation(key_, settled_state_);
        if (!advanced.accepted())
            return {OperationReconciliationStatus::persistence_failed, cycle,
                    journal_.find(key_)};
        return {OperationReconciliationStatus::progressed, cycle, advanced.record};
    }

    /// \brief Returns the latest observation worker cycle.
    /// \return Last cycle, or empty before the first step.
    const std::optional<ReconciliationWorkerCycle> &last_cycle() const {
        return worker_.last_cycle();
    }

private:
    static std::optional<ReconciliationDescriptor> make_descriptor(
        const OperationKey &key, const ReconciliationRequest &request,
        OperationState settled_state) {
        if (!request.baseline)
            return std::nullopt;
        ReconciliationDescriptor descriptor{key.account, *request.baseline,
                                            request.predicates, settled_state,
                                            key.trade_id, key.operation_id};
        return descriptor.valid()
                   ? std::optional<ReconciliationDescriptor>(std::move(descriptor))
                   : std::nullopt;
    }

    static std::optional<ReconciliationDescriptor> journal_descriptor(
        OperationJournal &journal, const OperationKey &key) {
        const auto record = journal.find(key);
        if (!record || !record->reconciliation_descriptor)
            return std::nullopt;
        return record->reconciliation_descriptor;
    }

    static ReconciliationRequest request_from_descriptor(
        const std::optional<ReconciliationDescriptor> &descriptor,
        const std::vector<ReconciliationBinding> &bindings,
        const ObservationGraph &graph) {
        ReconciliationRequest request;
        if (descriptor) {
            // Graph instance ids and domain revisions are process-local. A
            // recovered descriptor remains the immutable provenance contract,
            // so a recovered worker always starts from the current graph
            // baseline. The next refresh then supplies fresh evidence even if
            // a restarted process happens to reuse an instance id.
            request.baseline = capture_reconciliation_baseline(graph);
            request.predicates = descriptor->predicates;
            for (auto &predicate : request.predicates) {
                if (predicate.ticket != 0 || predicate.correlation_id == 0)
                    continue;
                for (const auto &binding : bindings) {
                    if (binding.correlation_id == predicate.correlation_id) {
                        predicate.ticket = binding.broker_ticket;
                        break;
                    }
                }
            }
        }
        return request;
    }

    static ReconciliationRequest request_from_journal(
        OperationJournal &journal, const OperationKey &key,
        const ObservationGraph &graph) {
        const auto record = journal.find(key);
        if (!record)
            return {};
        return request_from_descriptor(record->reconciliation_descriptor,
                                       record->reconciliation_bindings, graph);
    }

    static bool baseline_usable(const ReconciliationBaseline &baseline,
                                const ObservationGraph &graph) {
        return baseline.graph_instance_id() == graph.instance_id() &&
               baseline.graph_revision() <= graph.revision() &&
               baseline.active_orders_revision() <=
                   graph.domain_revision(ObservationDomain::active_orders) &&
               baseline.positions_revision() <=
                   graph.domain_revision(ObservationDomain::positions) &&
               baseline.history_orders_revision() <=
                   graph.domain_revision(ObservationDomain::history_orders) &&
               baseline.history_deals_revision() <=
                   graph.domain_revision(ObservationDomain::history_deals);
    }

    static ReconciliationRequest rebase_request(ReconciliationRequest request,
                                                const ObservationGraph &graph) {
        if (request.baseline && !baseline_usable(*request.baseline, graph))
            request.baseline = capture_reconciliation_baseline(graph);
        return request;
    }

    static bool same_window(const std::optional<ObservationWindow> &left,
                            const std::optional<ObservationWindow> &right) {
        if (left.has_value() != right.has_value())
            return false;
        return !left || (left->from_msc == right->from_msc &&
                         left->to_msc == right->to_msc);
    }

    static bool same_descriptor(
        const ReconciliationDescriptor &left, const ReconciliationDescriptor &right,
        const std::vector<ReconciliationBinding> &bindings) {
        if (left.account != right.account ||
            left.baseline.account() != right.baseline.account() ||
            left.baseline.graph_instance_id() != right.baseline.graph_instance_id() ||
            left.baseline.graph_revision() != right.baseline.graph_revision() ||
            left.baseline.active_orders_revision() != right.baseline.active_orders_revision() ||
            left.baseline.positions_revision() != right.baseline.positions_revision() ||
            left.baseline.history_orders_revision() != right.baseline.history_orders_revision() ||
            left.baseline.history_deals_revision() != right.baseline.history_deals_revision() ||
            left.settled_state != right.settled_state ||
            left.trade_id != right.trade_id || left.operation_id != right.operation_id ||
            left.predicates.size() != right.predicates.size())
            return false;
        for (std::size_t index = 0; index < left.predicates.size(); ++index) {
            const auto &left_predicate = left.predicates[index];
            const auto &right_predicate = right.predicates[index];
            const bool ticket_matches =
                left_predicate.ticket == right_predicate.ticket ||
                (left_predicate.correlation_id != 0 &&
                 left_predicate.correlation_id == right_predicate.correlation_id &&
                 (left_predicate.ticket == 0 || right_predicate.ticket == 0) &&
                 std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
                     return binding.correlation_id == left_predicate.correlation_id &&
                            binding.broker_ticket ==
                                (left_predicate.ticket == 0 ? right_predicate.ticket
                                                            : left_predicate.ticket);
                 }));
            if (left_predicate.kind != right_predicate.kind || !ticket_matches ||
                left_predicate.baseline_present != right_predicate.baseline_present ||
                left_predicate.correlation_id != right_predicate.correlation_id ||
                left_predicate.expected_transition !=
                    right_predicate.expected_transition ||
                !same_window(left_predicate.history_window,
                             right_predicate.history_window))
                return false;
        }
        return true;
    }

    static bool valid_settled_state(OperationState state) {
        return state == OperationState::partially_filled ||
               state == OperationState::filled || state == OperationState::cancelled ||
               state == OperationState::expired || state == OperationState::rejected;
    }

    static bool is_terminal(OperationState state) {
        return state == OperationState::filled || state == OperationState::cancelled ||
               state == OperationState::expired || state == OperationState::rejected ||
               state == OperationState::failed || state == OperationState::ambiguous;
    }

    JournalMutationResult normalize(const OperationRecord &record) {
        JournalMutationResult result{JournalMutationStatus::accepted, record};
        if (record.operation_state == OperationState::prechecking ||
            record.operation_state == OperationState::submitting ||
            record.operation_state == OperationState::accepted) {
            result = journal_.transition_operation(key_, OperationState::reconciling);
            if (!result.accepted())
                return result;
        }
        const auto current = journal_.find(key_);
        if (!current)
            return {JournalMutationStatus::not_found, std::nullopt};
        if (current->journal_state != JournalState::reconciling) {
            result = journal_.transition_journal(key_, JournalState::reconciling);
            if (!result.accepted())
                return result;
        }
        return result;
    }

    OperationJournal &journal_;
    OperationKey key_;
    std::optional<ReconciliationDescriptor> descriptor_;
    ReconciliationWorker worker_;
    OperationState settled_state_;
};

} // namespace mt5bridge

#pragma once

/// \file dispatch/operation_worker.hpp
/// \brief Defines journal-aware operation reconciliation.

#include "journal.hpp"
#include "operation_recovery.hpp" // Preserve the pre-extraction public include contract.
#include <mt5bridge/reconciliation/worker.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge {

/// \enum OperationReconciliationStatus
/// \brief Reports one journal-aware reconciliation cycle.
enum class OperationReconciliationStatus {
    confirmed,           ///< Predicates confirmed; lifecycle remains reconciling.
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

    /// \brief Tests whether predicates were confirmed without a terminal write.
    /// \return True for an observation-only confirmation.
    bool confirmed() const {
        return status == OperationReconciliationStatus::confirmed;
    }
};

/// \class OperationReconciliationWorker
/// \brief Couples bounded observation cycles to one durable operation.
///
/// The worker normalizes any recovered post-dispatch record to the
/// `reconciling` journal state before collecting evidence. `pending`,
/// `not_observed`, and event gaps never become terminal and never permit a
/// resend. The caller supplies explicit predicates and a candidate lifecycle
/// state; broker result payloads remain hints only. Fill states stay unresolved
/// until a separate provenance-bearing settlement path supplies semantic
/// executed-volume evidence.
/// Confirmation of `filled` or `partially_filled` is deliberately observation
/// only.  Those states require semantic settlement evidence (for example an
/// attributed executed-volume history), so a predicate such as
/// `active_order_present` cannot terminalize the journal by itself.
class OperationReconciliationWorker {
public:
    /// \brief Binds one operation to a caller-driven observation worker.
    /// \param journal Owner-loop journal containing the operation.
    /// \param key Immutable operation identity.
    /// \param coordinator Coordinator used for authoritative refreshes.
    /// \param collection_request Domains refreshed on each cycle.
    /// \param reconciliation_request Explicit evidence predicates and baseline.
    /// \param settled_state Candidate lifecycle state retained in the durable
    ///        descriptor. Generic confirmation never applies this state.
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
    /// \note A graph from a new process must already be bound to the operation
    ///       account before recovery starts. A foreign or unbound graph is
    ///       rejected without provider collection; the durable descriptor
    ///       itself remains unchanged.
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

    /// \brief Performs one refresh and applies safe durable transitions.
    /// \param trade_event_gap True when event hints lost continuity.
    /// \param deadline_expired True when the bounded wait elapsed.
    /// \return Evidence and the resulting durable lifecycle status.
    OperationReconciliationCycle step(bool trade_event_gap = false,
                                      bool deadline_expired = false) {
        return step_impl(trade_event_gap, deadline_expired);
    }

    /// \brief Returns the effective graph-local baseline used by reconciliation.
    /// \return Baseline rebased for the worker's current graph instance.
    const std::optional<ReconciliationBaseline> &effective_baseline() const {
        return worker_.baseline();
    }

private:
    OperationReconciliationCycle step_impl(bool trade_event_gap,
                                           bool deadline_expired) {
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

        if (!graph_matches_operation(worker_.graph(), key_))
            return {OperationReconciliationStatus::account_mismatch, std::nullopt,
                    journal_.find(key_)};

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

        return {OperationReconciliationStatus::confirmed, cycle,
                journal_.find(key_)};
    }

public:
    /// \brief Returns the latest observation worker cycle.
    /// \return Last cycle, or empty before the first step.
    const std::optional<ReconciliationWorkerCycle> &last_cycle() const {
        return worker_.last_cycle();
    }

    /// \brief Returns the immutable operation identity owned by this worker.
    /// \return Account-scoped durable operation key.
    const OperationKey &key() const { return key_; }

    /// \brief Returns the graph used to produce worker provenance.
    /// \return Read-only observation graph bound to this worker.
    const ObservationGraph &graph() const { return worker_.graph(); }

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
            // so a matching-account worker starts from the current graph
            // baseline. A foreign bound graph keeps the durable baseline and
            // is rejected by the owner loop before any provider refresh.
            request.baseline =
                !graph.bound() || graph.account_key() != descriptor->account
                    ? std::optional<ReconciliationBaseline>(descriptor->baseline)
                    : std::optional<ReconciliationBaseline>(
                          capture_reconciliation_baseline(graph));
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
        if (request.baseline && !baseline_usable(*request.baseline, graph) &&
            (graph.bound() && graph.account_key() == request.baseline->account()))
            request.baseline = capture_reconciliation_baseline(graph);
        return request;
    }

    static bool graph_matches_operation(const ObservationGraph &graph,
                                        const OperationKey &key) {
        return graph.bound() && graph.account_key() == key.account;
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

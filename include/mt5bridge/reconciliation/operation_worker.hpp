#pragma once

/// \file reconciliation/operation_worker.hpp
/// \brief Defines journal-aware operation reconciliation and startup recovery.

#include <mt5bridge/dispatch/journal.hpp>
#include <mt5bridge/observation/worker.hpp>

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
          worker_(coordinator, std::move(collection_request),
                  std::move(reconciliation_request)),
          settled_state_(settled_state) {}

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
    ReconciliationWorker worker_;
    OperationState settled_state_;
};

} // namespace mt5bridge

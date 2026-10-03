#pragma once

/// \file dispatch/operation_recovery.hpp
/// \brief Defines fail-closed classification of durable operation recovery.

#include "journal.hpp"

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
/// crash after the durable `dispatching` barrier and before the backend call.
class OperationRecoveryCoordinator {
public:
    /// \brief Recovers and classifies the complete journal atomically.
    /// \param journal Journal whose owner cache is replaced on success.
    /// \return Complete classification, or a failure with no partial result.
    static OperationRecoveryResult recover(OperationJournal &journal) {
        const auto recovered = journal.recover_all();
        if (!recovered.accepted())
            return {recovered.status(), {}};

        OperationRecoveryResult result;
        result.status = JournalMutationStatus::accepted;
        result.operations.reserve(recovered.records().size());
        for (const auto &record : recovered.records()) {
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

} // namespace mt5bridge

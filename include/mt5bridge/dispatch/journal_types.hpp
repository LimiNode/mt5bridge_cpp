#pragma once

/// \file dispatch/journal_types.hpp
/// \brief Defines durable operation identities, evidence descriptors, and journal records.

#include <mt5bridge/reconciliation/environment_consistency.hpp>
#include <mt5bridge/reconciliation/engine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge {

/// \typedef TradeId
/// \brief Stable logical trade identity supplied by the managed lifecycle.
using TradeId = std::uint64_t;

/// \typedef OperationId
/// \brief Stable identity of one side-effect attempt.
using OperationId = std::uint64_t;

/// \enum OperationState
/// \brief Durable lifecycle state of one managed operation.
enum class OperationState {
    queued,              ///< Intent exists but has not entered validation.
    prechecking,         ///< Capabilities and order_check are being evaluated.
    submitting,          ///< The side-effect call has entered its backend.
    accepted,            ///< Durable result payload contains acceptance evidence.
    reconciling,         ///< Snapshots are being used to resolve the outcome.
    partially_filled,    ///< Reconciliation proved a partial execution.
    filled,              ///< Reconciliation proved the requested execution complete.
    cancelled,           ///< The operation ended because it was cancelled.
    expired,             ///< The operation ended because its expiry was reached.
    rejected,            ///< Validation or the server rejected the operation.
    failed,              ///< The operation failed before a successful outcome.
    ambiguous,           ///< Evidence cannot uniquely attribute the outcome.
};

/// \struct ReconciliationDescriptor
/// \brief Immutable post-dispatch evidence contract retained in the journal.
///
/// The descriptor contains the pre-side-effect baseline, explicit predicates,
/// and lifecycle state that a confirmed reconciliation may settle. Deadline
/// and event-gap hints remain owner-loop inputs and are intentionally not
/// durable evidence.
struct ReconciliationDescriptor {
    AccountKey account; ///< Immutable account scope of the operation.
    ReconciliationBaseline baseline; ///< Graph baseline captured before dispatch.
    std::vector<ReconciliationPredicate> predicates; ///< Required evidence assertions.
    OperationState settled_state = OperationState::filled; ///< State proven on confirmation.
    std::uint64_t trade_id = 0; ///< Managed trade identity bound at persistence time.
    std::uint64_t operation_id = 0; ///< Side-effect identity bound at persistence time.

    /// \brief Tests whether the descriptor is safe to persist and replay.
    /// \return True only for a valid account, baseline, predicates, and target state.
    bool valid() const {
        if (!account.valid() || !baseline.valid() || account != baseline.account() ||
            predicates.empty())
            return false;
        if (settled_state != OperationState::partially_filled &&
            settled_state != OperationState::filled &&
            settled_state != OperationState::cancelled &&
            settled_state != OperationState::expired &&
            settled_state != OperationState::rejected)
            return false;
        for (std::size_t index = 0; index < predicates.size(); ++index) {
            const auto &predicate = predicates[index];
            if (!predicate.baseline_present ||
                predicate.expected_transition == ReconciliationTransition::unspecified ||
                (predicate.ticket == 0 && predicate.correlation_id == 0))
                return false;
            const bool history =
                predicate.kind == ReconciliationPredicateKind::history_order_present ||
                predicate.kind == ReconciliationPredicateKind::history_order_absent ||
                predicate.kind == ReconciliationPredicateKind::history_deal_present ||
                predicate.kind == ReconciliationPredicateKind::history_deal_absent;
            const bool absence =
                predicate.kind == ReconciliationPredicateKind::active_order_absent ||
                predicate.kind == ReconciliationPredicateKind::position_absent ||
                predicate.kind == ReconciliationPredicateKind::history_order_absent ||
                predicate.kind == ReconciliationPredicateKind::history_deal_absent;
            if (predicate.ticket == 0 && absence)
                return false;
            // A broker result has no position ticket. Until the observation
            // layer can resolve deal.position_id into a position identity,
            // fail closed instead of creating an unresolvable durable contract.
            if (predicate.ticket == 0 &&
                predicate.kind == ReconciliationPredicateKind::position_present)
                return false;
            switch (predicate.kind) {
            case ReconciliationPredicateKind::active_order_present:
            case ReconciliationPredicateKind::active_order_absent:
            case ReconciliationPredicateKind::position_present:
            case ReconciliationPredicateKind::position_absent:
            case ReconciliationPredicateKind::history_order_present:
            case ReconciliationPredicateKind::history_order_absent:
            case ReconciliationPredicateKind::history_deal_present:
            case ReconciliationPredicateKind::history_deal_absent:
                break;
            default:
                return false;
            }
            const bool presence = !absence;
            if ((presence &&
                 predicate.expected_transition !=
                     ReconciliationTransition::absent_to_present) ||
                (absence &&
                 predicate.expected_transition !=
                     ReconciliationTransition::present_to_absent) ||
                (predicate.expected_transition ==
                     ReconciliationTransition::absent_to_present &&
                 *predicate.baseline_present) ||
                (predicate.expected_transition ==
                     ReconciliationTransition::present_to_absent &&
                 !*predicate.baseline_present))
                return false;
            if ((!history && predicate.history_window) ||
                (history && predicate.history_window && !predicate.history_window->valid()) ||
                (absence && history && !predicate.history_window) ||
                (history && !*predicate.baseline_present && !predicate.history_window))
                return false;
            if (predicate.correlation_id != 0) {
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (predicates[prior].correlation_id == predicate.correlation_id)
                        return false;
                }
            }
        }
        return true;
    }
};

/// \struct ReconciliationBinding
/// \brief Durable single-assignment binding from client correlation to broker ticket.
struct ReconciliationBinding {
    std::uint64_t correlation_id = 0; ///< Client identity from the descriptor.
    std::uint64_t broker_ticket = 0; ///< Ticket supplied by a validated result.

    /// \brief Tests whether both identity components are usable.
    /// \return True only for a non-zero correlation and broker ticket.
    bool valid() const { return correlation_id != 0 && broker_ticket != 0; }

    /// \brief Compares two bindings for idempotent replay.
    /// \param other Binding to compare.
    /// \return True when both identities match exactly.
    bool operator==(const ReconciliationBinding &other) const {
        return correlation_id == other.correlation_id &&
               broker_ticket == other.broker_ticket;
    }
};

/// \brief Tests whether every unknown predicate has one durable ticket binding.
/// \param descriptor Descriptor whose result-derived identities are required.
/// \param bindings Candidate bindings extracted from one broker result.
/// \return True only when the binding set is complete for the descriptor.
inline bool descriptor_bindings_complete(
    const ReconciliationDescriptor &descriptor,
    const std::vector<ReconciliationBinding> &bindings) {
    std::size_t required = 0;
    for (const auto &predicate : descriptor.predicates) {
        if (predicate.ticket == 0)
            ++required;
    }
    return required == bindings.size();
}

/// \enum JournalState
/// \brief Durable write-ahead state around the future side effect.
enum class JournalState {
    created,                 ///< Operation intent has been durably created.
    prechecked,              ///< Advisory checks have been durably recorded.
    dispatch_intent_persisted, ///< Request payload is ready for admission.
    dispatching,             ///< Non-resendable may-have-been-sent barrier.
    result_persisted,        ///< Raw backend result is durably recorded.
    reconciling,             ///< Snapshot reconciliation is in progress.
};

/// \brief Tests whether a value is a defined operation lifecycle state.
/// \param state Candidate operation state, possibly recovered from storage.
/// \return True only for a known enumerator.
constexpr bool valid_operation_state(OperationState state) {
    switch (state) {
    case OperationState::queued:
    case OperationState::prechecking:
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
        return true;
    }
    return false;
}

/// \brief Tests whether a value is a defined write-ahead journal state.
/// \param state Candidate journal state, possibly recovered from storage.
/// \return True only for a known enumerator.
constexpr bool valid_journal_state(JournalState state) {
    switch (state) {
    case JournalState::created:
    case JournalState::prechecked:
    case JournalState::dispatch_intent_persisted:
    case JournalState::dispatching:
    case JournalState::result_persisted:
    case JournalState::reconciling:
        return true;
    }
    return false;
}

/// \brief Tests whether a journal state is at or beyond the dispatch barrier.
/// \param state Candidate journal state.
/// \return True for `dispatching`, `result_persisted`, or `reconciling`.
constexpr bool journal_at_least_dispatching(JournalState state) {
    return state == JournalState::dispatching ||
           state == JournalState::result_persisted ||
           state == JournalState::reconciling;
}

/// \brief Tests whether a journal state follows durable result persistence.
/// \param state Candidate journal state.
/// \return True for `result_persisted` or `reconciling`.
constexpr bool journal_at_least_result_persisted(JournalState state) {
    return state == JournalState::result_persisted ||
           state == JournalState::reconciling;
}

/// \brief Tests whether a journal transition requires prechecking.
/// \param state Candidate next journal state.
/// \return True for the prechecking-to-dispatching transition segment.
constexpr bool journal_requires_prechecking(JournalState state) {
    return state == JournalState::prechecked ||
           state == JournalState::dispatch_intent_persisted ||
           state == JournalState::dispatching;
}

/// \brief Tests whether journal and operation state form a recoverable pair.
/// \param journal_state Durable write-ahead state.
/// \param operation_state Canonical managed-operation state.
/// \return True only for combinations admitted by the staged lifecycle.
constexpr bool valid_state_pair(JournalState journal_state,
                                OperationState operation_state);

/// \struct OperationKey
/// \brief Account-scoped identity used to address one journal operation.
struct OperationKey {
    AccountKey account; ///< Immutable terminal identity.
    TradeId trade_id = 0; ///< Managed logical trade identity.
    OperationId operation_id = 0; ///< One side-effect attempt identity.

    /// \brief Tests whether every identity component is usable.
    /// \return True when the account and both IDs are non-zero/valid.
    bool valid() const {
        return account.valid() && trade_id != 0 && operation_id != 0;
    }

    /// \brief Compares two operation keys for exact identity.
    /// \param other Key to compare.
    /// \return True when account and managed IDs match.
    bool operator==(const OperationKey &other) const {
        return account == other.account && trade_id == other.trade_id &&
               operation_id == other.operation_id;
    }

    /// \brief Compares two operation keys for inequality.
    /// \param other Key to compare.
    /// \return True when any identity component differs.
    bool operator!=(const OperationKey &other) const { return !(*this == other); }

    /// \brief Orders operation keys for deterministic journal storage.
    /// \param other Key to compare.
    /// \return True when this key sorts before `other`.
    bool operator<(const OperationKey &other) const {
        if (account.server != other.account.server)
            return account.server < other.account.server;
        if (account.login != other.account.login)
            return account.login < other.account.login;
        if (trade_id != other.trade_id)
            return trade_id < other.trade_id;
        return operation_id < other.operation_id;
    }
};

/// \struct OperationRecord
/// \brief Durable operation intent and lifecycle state.
struct OperationRecord {
    OperationKey key; ///< Immutable account and managed-operation identity.
    std::vector<std::uint8_t> request_payload; ///< Exact opaque request bytes.
    std::vector<std::uint8_t> result_payload; ///< Opaque backend result, when persisted.
    OperationState operation_state = OperationState::queued; ///< Managed lifecycle state.
    JournalState journal_state = JournalState::created; ///< Write-ahead state.
    std::uint64_t revision = 0; ///< Monotonic journal revision for this record.
    std::uint64_t fencing_token = 0; ///< Writer token committed at dispatching.
    std::optional<ReconciliationDescriptor>
        reconciliation_descriptor; ///< Durable post-dispatch evidence contract.
    std::vector<ReconciliationBinding>
        reconciliation_bindings; ///< Durable broker identities derived from result evidence.

    /// \brief Tests whether the record can be persisted or recovered safely.
    /// \return True when identity, payload, state pair, revision, and fencing agree.
    bool valid() const {
        if (!key.valid() || request_payload.empty() || revision == 0 ||
            !valid_operation_state(operation_state) || !valid_journal_state(journal_state))
            return false;
        if (!valid_state_pair(journal_state, operation_state))
            return false;
        if (reconciliation_descriptor &&
            (!reconciliation_descriptor->valid() ||
             reconciliation_descriptor->account != key.account ||
             (reconciliation_descriptor->trade_id != 0 &&
              reconciliation_descriptor->trade_id != key.trade_id) ||
             (reconciliation_descriptor->operation_id != 0 &&
              reconciliation_descriptor->operation_id != key.operation_id)))
            return false;
        if (journal_at_least_dispatching(journal_state) &&
            (!reconciliation_descriptor || reconciliation_descriptor->trade_id == 0 ||
             reconciliation_descriptor->operation_id == 0 ||
             reconciliation_descriptor->trade_id != key.trade_id ||
             reconciliation_descriptor->operation_id != key.operation_id))
            return false;
        if (!journal_at_least_result_persisted(journal_state) && !result_payload.empty())
            return false;
        if (journal_state == JournalState::result_persisted && result_payload.empty())
            return false;
        if (operation_state == OperationState::accepted && result_payload.empty())
            return false;
        if (!reconciliation_bindings.empty()) {
            if (!reconciliation_descriptor ||
                !journal_at_least_result_persisted(journal_state) ||
                result_payload.empty())
                return false;
            for (std::size_t index = 0; index < reconciliation_bindings.size(); ++index) {
                const auto &binding = reconciliation_bindings[index];
                if (!binding.valid())
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (reconciliation_bindings[prior].correlation_id ==
                            binding.correlation_id ||
                        reconciliation_bindings[prior].broker_ticket == binding.broker_ticket)
                        return false;
                }
                const auto predicate = std::find_if(
                    reconciliation_descriptor->predicates.begin(),
                    reconciliation_descriptor->predicates.end(),
                    [&binding](const auto &candidate) {
                        return candidate.correlation_id == binding.correlation_id;
                    });
                if (predicate == reconciliation_descriptor->predicates.end() ||
                    predicate->ticket != 0)
                    return false;
            }
        }
        if (reconciliation_descriptor &&
            journal_at_least_result_persisted(journal_state) &&
            !(journal_state == JournalState::result_persisted &&
              operation_state == OperationState::rejected) &&
            !descriptor_bindings_complete(*reconciliation_descriptor,
                                          reconciliation_bindings))
            return false;
        if (!journal_at_least_dispatching(journal_state))
            return fencing_token == 0;
        return fencing_token != 0;
    }
};

constexpr bool valid_state_pair(JournalState journal_state,
                                OperationState operation_state) {
    switch (journal_state) {
    case JournalState::created:
        return operation_state == OperationState::queued ||
               operation_state == OperationState::prechecking ||
               operation_state == OperationState::rejected ||
               operation_state == OperationState::failed;
    case JournalState::prechecked:
        return operation_state == OperationState::prechecking ||
               operation_state == OperationState::rejected ||
               operation_state == OperationState::failed;
    case JournalState::dispatch_intent_persisted:
        return operation_state == OperationState::prechecking;
    case JournalState::dispatching:
        return operation_state == OperationState::prechecking ||
               operation_state == OperationState::submitting ||
               operation_state == OperationState::reconciling ||
               operation_state == OperationState::ambiguous;
    case JournalState::result_persisted:
        return operation_state == OperationState::submitting ||
               operation_state == OperationState::accepted ||
               operation_state == OperationState::rejected ||
               operation_state == OperationState::reconciling ||
               operation_state == OperationState::ambiguous;
    case JournalState::reconciling:
        return operation_state == OperationState::prechecking ||
               operation_state == OperationState::submitting ||
               operation_state == OperationState::accepted ||
               operation_state == OperationState::reconciling ||
               operation_state == OperationState::partially_filled ||
               operation_state == OperationState::filled ||
               operation_state == OperationState::cancelled ||
               operation_state == OperationState::expired ||
               operation_state == OperationState::rejected ||
               operation_state == OperationState::ambiguous;
    }
    return false;
}

} // namespace mt5bridge

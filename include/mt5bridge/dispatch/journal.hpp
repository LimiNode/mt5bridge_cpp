#pragma once

/// \file dispatch/journal.hpp
/// \brief Defines the durable operation journal and pre-side-effect admission barrier.

#include <mt5bridge/reconciliation/environment_consistency.hpp>
#include <mt5bridge/reconciliation/engine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge::runtime {
class OneShotDispatchBackend;
}

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
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

/// \brief Verifies descriptor provenance against the graph used for admission.
/// \param descriptor Durable descriptor to validate.
/// \param graph Graph whose current revision is about to be admitted.
/// \return True only when the baseline is exact and known-ticket state agrees.
inline bool reconciliation_descriptor_matches_graph(
    const ReconciliationDescriptor &descriptor, const ObservationGraph &graph) {
    if (!descriptor.valid() || !graph.bound() ||
        descriptor.account != graph.account_key() ||
        descriptor.baseline.account() != graph.account_key() ||
        descriptor.baseline.graph_instance_id() != graph.instance_id() ||
        descriptor.baseline.graph_revision() != graph.revision() ||
        descriptor.baseline.active_orders_revision() !=
            graph.domain_revision(ObservationDomain::active_orders) ||
        descriptor.baseline.positions_revision() !=
            graph.domain_revision(ObservationDomain::positions) ||
        descriptor.baseline.history_orders_revision() !=
            graph.domain_revision(ObservationDomain::history_orders) ||
        descriptor.baseline.history_deals_revision() !=
            graph.domain_revision(ObservationDomain::history_deals))
        return false;

    const auto contains = [](const auto &values, std::uint64_t ticket) {
        for (const auto &value : values) {
            if (value.ticket == ticket)
                return true;
        }
        return false;
    };
    const auto active_orders = graph.active_orders();
    const auto positions = graph.positions();
    const auto history_orders = graph.history_orders();
    const auto history_deals = graph.history_deals();
    for (const auto &predicate : descriptor.predicates) {
        const bool baseline_present = *predicate.baseline_present;
        if (!baseline_present) {
            switch (predicate.kind) {
            case ReconciliationPredicateKind::active_order_present:
            case ReconciliationPredicateKind::active_order_absent:
                if (graph.domain_revision(ObservationDomain::active_orders) == 0)
                    return false;
                break;
            case ReconciliationPredicateKind::position_present:
            case ReconciliationPredicateKind::position_absent:
                if (graph.domain_revision(ObservationDomain::positions) == 0)
                    return false;
                break;
            case ReconciliationPredicateKind::history_order_present:
            case ReconciliationPredicateKind::history_order_absent:
                if (!predicate.history_window ||
                    !graph.history_orders_covered_at(
                        *predicate.history_window,
                        descriptor.baseline.graph_revision()))
                    return false;
                break;
            case ReconciliationPredicateKind::history_deal_present:
            case ReconciliationPredicateKind::history_deal_absent:
                if (!predicate.history_window ||
                    !graph.history_deals_covered_at(
                        *predicate.history_window,
                        descriptor.baseline.graph_revision()))
                    return false;
                break;
            }
        }
        if (predicate.ticket == 0)
            continue;
        bool present = false;
        switch (predicate.kind) {
        case ReconciliationPredicateKind::active_order_present:
        case ReconciliationPredicateKind::active_order_absent:
            present = contains(active_orders, predicate.ticket);
            break;
        case ReconciliationPredicateKind::position_present:
        case ReconciliationPredicateKind::position_absent:
            present = contains(positions, predicate.ticket);
            break;
        case ReconciliationPredicateKind::history_order_present:
        case ReconciliationPredicateKind::history_order_absent:
            present = contains(history_orders, predicate.ticket);
            break;
        case ReconciliationPredicateKind::history_deal_present:
        case ReconciliationPredicateKind::history_deal_absent:
            present = contains(history_deals, predicate.ticket);
            break;
        }
        if (present != baseline_present)
            return false;
    }
    return true;
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

/// \enum StoreCommitStatus
/// \brief Reports the result of one compare-and-commit journal write.
enum class StoreCommitStatus {
    committed, ///< Candidate replaced the expected durable record.
    conflict,  ///< Expected revision/absence no longer matches durable state.
    io_error,  ///< The store could not durably commit the candidate.
};

/// \enum StoreLoadStatus
/// \brief Reports the result of loading one durable operation record.
enum class StoreLoadStatus {
    found,          ///< A complete valid record was loaded.
    not_found,      ///< No record exists for the requested key.
    invalid_record, ///< Storage exists but its record is malformed or inconsistent.
    io_error,       ///< The store could not complete the read.
};

/// \struct StoreLoadResult
/// \brief Carries a status-bearing single-record load result.
struct StoreLoadResult {
    StoreLoadStatus status = StoreLoadStatus::io_error;
    std::optional<OperationRecord> record;

    /// \brief Tests whether a complete record was loaded.
    /// \return True only when `record` is present and the status is `found`.
    bool found() const {
        return status == StoreLoadStatus::found && record.has_value();
    }
};

/// \enum StoreScanStatus
/// \brief Reports the result of enumerating durable operation records.
enum class StoreScanStatus {
    complete,       ///< Every operation record was loaded and validated.
    invalid_record, ///< At least one record is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \struct StoreScanResult
/// \brief Carries an all-or-nothing durable record enumeration.
struct StoreScanResult {
    StoreScanStatus status = StoreScanStatus::io_error;
    std::vector<OperationRecord> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when all discovered records are present and valid.
    bool complete() const { return status == StoreScanStatus::complete; }
};

/// \class DurableJournalStore
/// \brief Persistence seam whose compare-and-commit returns only after durable storage.
class DurableJournalStore {
public:
    virtual ~DurableJournalStore() = default;

    /// \brief Compare-and-commits one operation record durably.
    /// \param record Complete next record, including its incremented revision.
    /// \param expected_revision Expected current revision; empty means no record exists.
    /// \return Commit, conflict, or I/O status.
    /// \warning Returning `committed` before the record survives a process crash breaks
    /// the non-resendable dispatch barrier.
    virtual StoreCommitStatus commit(
        const OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) = 0;

    /// \brief Loads the last durable record for one operation.
    /// \param key Account-scoped operation identity.
    /// \return A status that distinguishes absence from invalid storage or I/O.
    virtual StoreLoadResult load(const OperationKey &key) const = 0;

    /// \brief Enumerates every durable operation record atomically for recovery.
    /// \return A complete record set, or a failure with no usable partial set.
    virtual StoreScanResult scan() const = 0;
};

/// \enum JournalMutationStatus
/// \brief Reports the result of a journal mutation or recovery operation.
enum class JournalMutationStatus {
    accepted,             ///< The requested record is now durable.
    invalid_record,       ///< Identity, payload, or record metadata is invalid.
    duplicate_operation,  ///< This operation key already has a durable record.
    not_found,            ///< No in-memory or durable record exists for the key.
    invalid_transition,   ///< The requested lifecycle transition is not allowed.
    conflict,             ///< A stale owner lost the compare-and-commit race.
    not_durable,          ///< The store rejected the proposed durable commit.
    storage_error,        ///< Durable storage could not be read during recovery.
};

/// \struct JournalMutationResult
/// \brief Returns the durable record produced by a journal operation.
struct JournalMutationResult {
    JournalMutationStatus status = JournalMutationStatus::invalid_record;
    std::optional<OperationRecord> record;

    /// \brief Tests whether the requested mutation was durably accepted.
    /// \return True only when `record` contains the committed value.
    bool accepted() const {
        return status == JournalMutationStatus::accepted && record.has_value();
    }
};

/// \struct JournalRecoveryResult
/// \brief Returns an all-or-nothing owner-loop recovery result.
struct JournalRecoveryResult {
    JournalMutationStatus status = JournalMutationStatus::invalid_record;
    std::vector<OperationRecord> records;

    /// \brief Tests whether the owner cache was replaced by a complete scan.
    /// \return True only when the scan was accepted.
    bool accepted() const { return status == JournalMutationStatus::accepted; }
};

/// \class OperationJournal
/// \brief Applies a fail-closed operation state machine over durable records.
///
/// The journal owns only the current owner-loop cache. Every accepted create or
/// transition first commits the complete candidate record through
/// `DurableJournalStore`; the cache changes only after that commit succeeds.
/// Recovery loads an already durable record into the owner loop. This slice
/// persists intent and the `dispatching` barrier but deliberately does not
/// invoke a backend or expose `order_send`.
class OperationJournal {
public:
    /// \brief Binds the journal to a caller-owned durable store.
    /// \param store Store used for every durable mutation and recovery.
    explicit OperationJournal(DurableJournalStore &store) : store_(store) {}

    OperationJournal(const OperationJournal &) = delete;
    OperationJournal &operator=(const OperationJournal &) = delete;

    /// \brief Creates and durably records a queued operation.
    /// \param key Account and managed-operation identity.
    /// \param request_payload Exact opaque request bytes retained for replay.
    /// \return Mutation status and the committed queued record.
    JournalMutationResult create(
        const OperationKey &key, std::vector<std::uint8_t> request_payload) {
        if (!key.valid() || request_payload.empty())
            return {};
        if (records_.find(key) != records_.end())
            return {JournalMutationStatus::duplicate_operation, std::nullopt};

        OperationRecord candidate;
        candidate.key = key;
        candidate.request_payload = std::move(request_payload);
        candidate.operation_state = OperationState::queued;
        candidate.journal_state = JournalState::created;
        candidate.revision = 1;
        if (!candidate.valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        const auto commit_status = store_.commit(candidate, std::nullopt);
        if (commit_status != StoreCommitStatus::committed)
            return {commit_status == StoreCommitStatus::conflict
                        ? JournalMutationStatus::conflict
                        : JournalMutationStatus::not_durable,
                    std::nullopt};
        records_.emplace(key, candidate);
        return {JournalMutationStatus::accepted, std::move(candidate)};
    }

    /// \brief Recovers one operation from the durable store into this owner loop.
    /// \param key Account and managed-operation identity.
    /// \return Recovery status and the loaded record.
    JournalMutationResult recover(const OperationKey &key) {
        if (!key.valid())
            return {};
        const auto loaded = store_.load(key);
        switch (loaded.status) {
        case StoreLoadStatus::not_found:
            return {JournalMutationStatus::not_found, std::nullopt};
        case StoreLoadStatus::invalid_record:
            return {JournalMutationStatus::invalid_record, std::nullopt};
        case StoreLoadStatus::io_error:
            return {JournalMutationStatus::storage_error, std::nullopt};
        case StoreLoadStatus::found:
            break;
        }
        if (!loaded.record || loaded.record->key != key || !loaded.record->valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        records_[key] = *loaded.record;
        return {JournalMutationStatus::accepted, loaded.record};
    }

    /// \brief Recovers every durable operation after a process restart.
    /// \return Complete replacement records, or a failure without cache mutation.
    JournalRecoveryResult recover_all() {
        const auto scanned = store_.scan();
        if (scanned.status == StoreScanStatus::invalid_record)
            return {JournalMutationStatus::invalid_record, {}};
        if (scanned.status == StoreScanStatus::io_error)
            return {JournalMutationStatus::storage_error, {}};

        std::map<OperationKey, OperationRecord> staged;
        for (const auto &record : scanned.records) {
            if (!record.key.valid() || !record.valid() ||
                !staged.emplace(record.key, record).second)
                return {JournalMutationStatus::invalid_record, {}};
        }
        std::vector<OperationRecord> recovered;
        recovered.reserve(staged.size());
        for (const auto &entry : staged)
            recovered.push_back(entry.second);
        records_.swap(staged);
        return {JournalMutationStatus::accepted, std::move(recovered)};
    }

    /// \brief Reads a record already owned by this journal loop.
    /// \param key Account and managed-operation identity.
    /// \return Cached record, or empty until create/recover succeeds.
    std::optional<OperationRecord> find(const OperationKey &key) const {
        const auto it = records_.find(key);
        return it == records_.end() ? std::nullopt
                                    : std::optional<OperationRecord>(it->second);
    }

    /// \brief Durably advances the write-ahead state machine.
    /// \param key Account and managed-operation identity.
    /// \param next_state Journal state requested by the owner loop.
    /// \param fencing_token Non-zero writer token required for `dispatching`.
    /// \return Mutation status and the committed next record.
    JournalMutationResult transition_journal(const OperationKey &key,
                                             JournalState next_state,
                                             std::uint64_t fencing_token = 0) {
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (!can_transition_journal(it->second.journal_state, next_state))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (next_state == JournalState::result_persisted)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (journal_requires_prechecking(next_state) &&
            it->second.operation_state != OperationState::prechecking)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (next_state == JournalState::dispatching && fencing_token == 0)
            return {JournalMutationStatus::invalid_record, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        candidate.journal_state = next_state;
        candidate.revision += 1;
        if (next_state == JournalState::dispatching)
            candidate.fencing_token = fencing_token;
        if (!candidate.valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        const auto commit_status = store_.commit(candidate, it->second.revision);
        if (commit_status != StoreCommitStatus::committed)
            return {commit_status == StoreCommitStatus::conflict
                        ? JournalMutationStatus::conflict
                        : JournalMutationStatus::not_durable,
                    std::nullopt};
        it->second = candidate;
        return {JournalMutationStatus::accepted, std::move(candidate)};
    }

    /// \brief Durably attaches the immutable post-dispatch evidence contract.
    /// \param key Account and managed-operation identity.
    /// \param descriptor Baseline, predicates, and settlement target to retain.
    /// \return Mutation status and the descriptor-bearing record.
    /// \note A descriptor can be attached only once and cannot be replaced.
    JournalMutationResult persist_reconciliation_descriptor(
        const OperationKey &key, ReconciliationDescriptor descriptor) {
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (it->second.journal_state != JournalState::dispatch_intent_persisted ||
            it->second.operation_state != OperationState::prechecking ||
            it->second.reconciliation_descriptor ||
            !descriptor.valid() || descriptor.account != key.account)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if ((descriptor.trade_id != 0 && descriptor.trade_id != key.trade_id) ||
            (descriptor.operation_id != 0 && descriptor.operation_id != key.operation_id))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        if (descriptor.trade_id == 0)
            descriptor.trade_id = key.trade_id;
        if (descriptor.operation_id == 0)
            descriptor.operation_id = key.operation_id;
        candidate.reconciliation_descriptor = std::move(descriptor);
        candidate.revision += 1;
        if (!candidate.valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        const auto commit_status = store_.commit(candidate, it->second.revision);
        if (commit_status != StoreCommitStatus::committed)
            return {commit_status == StoreCommitStatus::conflict
                        ? JournalMutationStatus::conflict
                        : JournalMutationStatus::not_durable,
                    std::nullopt};
        it->second = candidate;
        return {JournalMutationStatus::accepted, std::move(candidate)};
    }

    /// \brief Atomically persists a backend result without identity bindings.
    /// \param key Account and managed-operation identity.
    /// \param result_payload Exact opaque backend result bytes.
    /// \return Mutation status and the committed result-bearing record.
    JournalMutationResult persist_result(
        const OperationKey &key, std::vector<std::uint8_t> result_payload) {
        return persist_result_with_bindings(key, std::move(result_payload), {});
    }

    /// \brief Durably advances the managed operation lifecycle state.
    /// \param key Account and managed-operation identity.
    /// \param next_state Operation state requested by the owner loop.
    /// \return Mutation status and the committed next record.
    JournalMutationResult transition_operation(const OperationKey &key,
                                               OperationState next_state) {
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (!can_transition_operation(it->second.operation_state, next_state))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (next_state == OperationState::submitting &&
            it->second.journal_state != JournalState::dispatching)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if ((next_state == OperationState::reconciling ||
             next_state == OperationState::ambiguous) &&
            it->second.operation_state == OperationState::prechecking &&
            !journal_at_least_dispatching(it->second.journal_state))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (next_state == OperationState::accepted &&
            (!journal_at_least_result_persisted(it->second.journal_state) ||
             it->second.result_payload.empty()))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (next_state == OperationState::rejected &&
            it->second.operation_state == OperationState::submitting &&
            (!journal_at_least_result_persisted(it->second.journal_state) ||
             it->second.result_payload.empty()))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        candidate.operation_state = next_state;
        candidate.revision += 1;
        if (!candidate.valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        const auto commit_status = store_.commit(candidate, it->second.revision);
        if (commit_status != StoreCommitStatus::committed)
            return {commit_status == StoreCommitStatus::conflict
                        ? JournalMutationStatus::conflict
                        : JournalMutationStatus::not_durable,
                    std::nullopt};
        it->second = candidate;
        return {JournalMutationStatus::accepted, std::move(candidate)};
    }

    /// \brief Tests whether a write-ahead transition is legal.
    /// \param current Current durable journal state.
    /// \param next Proposed next journal state.
    /// \return True when the journal state machine permits the edge.
    static bool can_transition_journal(JournalState current, JournalState next) {
        switch (current) {
        case JournalState::created:
            return next == JournalState::prechecked;
        case JournalState::prechecked:
            return next == JournalState::dispatch_intent_persisted;
        case JournalState::dispatch_intent_persisted:
            return next == JournalState::dispatching;
        case JournalState::dispatching:
            return next == JournalState::result_persisted ||
                   next == JournalState::reconciling;
        case JournalState::result_persisted:
            return next == JournalState::reconciling;
        case JournalState::reconciling:
            return false;
        }
        return false;
    }

    /// \brief Tests whether a managed lifecycle transition is legal.
    /// \param current Current durable state.
    /// \param next Proposed next state.
    /// \return True when the state machine permits the edge.
    static bool can_transition_operation(OperationState current, OperationState next) {
        switch (current) {
        case OperationState::queued:
            return next == OperationState::prechecking || next == OperationState::failed;
        case OperationState::prechecking:
            return next == OperationState::submitting ||
                   next == OperationState::reconciling ||
                   next == OperationState::ambiguous ||
                   next == OperationState::rejected || next == OperationState::failed;
        case OperationState::submitting:
            return next == OperationState::accepted ||
                   next == OperationState::rejected ||
                   next == OperationState::reconciling ||
                   next == OperationState::ambiguous;
        case OperationState::accepted:
            return next == OperationState::reconciling;
        case OperationState::reconciling:
            return next == OperationState::partially_filled ||
                   next == OperationState::filled || next == OperationState::cancelled ||
                   next == OperationState::expired || next == OperationState::rejected ||
                   next == OperationState::ambiguous;
        case OperationState::partially_filled:
            return next == OperationState::reconciling ||
                   next == OperationState::filled || next == OperationState::cancelled ||
                   next == OperationState::expired || next == OperationState::ambiguous;
        case OperationState::filled:
        case OperationState::cancelled:
        case OperationState::expired:
        case OperationState::rejected:
        case OperationState::failed:
        case OperationState::ambiguous:
            return false;
        }
        return false;
    }

private:
    /// \brief Atomically persists a backend result and its identity bindings.
    /// \param key Account and managed-operation identity.
    /// \param result_payload Exact opaque backend result bytes.
    /// \param bindings Broker identities extracted from the same validated result.
    /// \return Mutation status and the committed result-bearing record.
    /// \note This mutation is private to the one-shot backend so application code
    /// cannot invent broker identity bindings.
    JournalMutationResult persist_result_with_bindings(
        const OperationKey &key, std::vector<std::uint8_t> result_payload,
        std::vector<ReconciliationBinding> bindings) {
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (it->second.journal_state != JournalState::dispatching ||
            result_payload.empty() ||
            it->second.operation_state != OperationState::submitting)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        candidate.journal_state = JournalState::result_persisted;
        candidate.result_payload = std::move(result_payload);
        candidate.reconciliation_bindings = std::move(bindings);
        candidate.revision += 1;
        if (!candidate.valid())
            return {JournalMutationStatus::invalid_record, std::nullopt};
        const auto commit_status = store_.commit(candidate, it->second.revision);
        if (commit_status != StoreCommitStatus::committed)
            return {commit_status == StoreCommitStatus::conflict
                        ? JournalMutationStatus::conflict
                        : JournalMutationStatus::not_durable,
                    std::nullopt};
        it->second = candidate;
        return {JournalMutationStatus::accepted, std::move(candidate)};
    }

    DurableJournalStore &store_;
    std::map<OperationKey, OperationRecord> records_;

    friend class runtime::OneShotDispatchBackend;
};

/// \class SingleWriterLease
/// \brief Verifies continuous ownership for one account-scoped dispatch.
class SingleWriterLease {
public:
    virtual ~SingleWriterLease() = default;

    /// \brief Reads the held fencing token for the immutable operation account.
    /// \param account Account that the caller is about to dispatch.
    /// \return Non-zero token only while this owner holds the exclusive lease.
    /// \note Implementations must perform the ownership check and token read as
    /// one indivisible operation, or use a fencing protocol that makes the
    /// returned token authoritative for the following durable commit.
    virtual std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &account) const = 0;
};

/// \struct DispatchAdmissionRequest
/// \brief Fresh evidence and blockers checked immediately before the barrier.
struct DispatchAdmissionRequest {
    AccountKey current_account; ///< Account read immediately before admission.
    std::optional<EnvironmentConsistencyProof>
        environment_proof; ///< Revision-bound topology proof.
    bool unresolved_operation = false; ///< Another operation is unresolved.
    bool event_gap = false; ///< A hint stream requires a fresh authoritative read.
};

/// \enum DispatchAdmissionStatus
/// \brief Reports why the durable dispatch barrier did or did not open.
enum class DispatchAdmissionStatus {
    admitted,             ///< `dispatching` was durably committed.
    invalid_request,      ///< Identity or admission evidence is malformed.
    operation_not_found,  ///< This owner loop has not created/recovered the key.
    invalid_state,         ///< The operation is not at the pre-side-effect edge.
    environment_not_ready, ///< The bounded topology proof is not consistent.
    account_mismatch,     ///< Current/evidence account differs from operation key.
    unresolved_operation,  ///< A prior operation blocks new dispatch.
    event_gap,             ///< An incomplete event hint stream blocks dispatch.
    lease_not_held,        ///< The fencing lease is absent or has no token.
    conflict,              ///< A stale owner lost the durable admission race.
    not_durable,           ///< The dispatching barrier could not be committed.
};

/// \struct DispatchPermit
/// \brief Non-resendable barrier evidence returned before a future backend call.
struct DispatchPermit {
public:
    DispatchPermit(const DispatchPermit &) = delete;
    DispatchPermit &operator=(const DispatchPermit &) = delete;
    /// \brief Transfers the one-shot permit and invalidates the source.
    /// \param other Permit whose capability is transferred.
    DispatchPermit(DispatchPermit &&other)
        : key_(std::move(other.key_)),
          fencing_token_(std::exchange(other.fencing_token_, 0)),
          journal_revision_(std::exchange(other.journal_revision_, 0)) {}
    /// \brief Transfers a one-shot permit and invalidates the source.
    /// \param other Permit whose capability is transferred.
    DispatchPermit &operator=(DispatchPermit &&other) {
        if (this != &other) {
            key_ = std::move(other.key_);
            fencing_token_ = std::exchange(other.fencing_token_, 0);
            journal_revision_ = std::exchange(other.journal_revision_, 0);
        }
        return *this;
    }

    /// \brief Tests whether the permit carries usable barrier evidence.
    /// \return True when identity, token, and revision are all present.
    bool valid() const {
        return key_.valid() && fencing_token_ != 0 && journal_revision_ != 0;
    }

    /// \brief Returns the protected operation identity.
    /// \return Account-scoped operation key.
    const OperationKey &key() const { return key_; }

    /// \brief Returns the committed fencing token.
    /// \return Non-zero writer token.
    std::uint64_t fencing_token() const { return fencing_token_; }

    /// \brief Returns the journal revision at the durable barrier.
    /// \return Monotonic operation revision.
    std::uint64_t journal_revision() const { return journal_revision_; }

private:
    DispatchPermit(OperationKey key, std::uint64_t fencing_token,
                   std::uint64_t journal_revision)
        : key_(std::move(key)), fencing_token_(fencing_token),
          journal_revision_(journal_revision) {}

    static DispatchPermit create(OperationKey key, std::uint64_t fencing_token,
                                 std::uint64_t journal_revision) {
        return DispatchPermit(std::move(key), fencing_token, journal_revision);
    }

    OperationKey key_;
    std::uint64_t fencing_token_ = 0;
    std::uint64_t journal_revision_ = 0;

    friend class DispatchAdmissionBarrier;
};

/// \struct DispatchAdmissionResult
/// \brief Reports the pre-side-effect admission decision.
struct DispatchAdmissionResult {
    DispatchAdmissionStatus status = DispatchAdmissionStatus::invalid_request;
    std::optional<DispatchPermit> permit;

    /// \brief Tests whether a durable non-resendable barrier was opened.
    /// \return True only when a valid permit is present.
    bool admitted() const {
        return status == DispatchAdmissionStatus::admitted && permit.has_value() &&
               permit->valid();
    }
};

/// \class DispatchAdmissionBarrier
/// \brief Opens the durable `dispatching` barrier without invoking `order_send`.
class DispatchAdmissionBarrier {
public:
    /// \brief Binds the barrier to one owner-loop journal, graph, and scope.
    /// \param journal Journal that owns the operation state transitions.
    /// \param graph Current graph whose revision must match the proof.
    /// \param required_scope Domains and history range required for admission.
    DispatchAdmissionBarrier(OperationJournal &journal, const ObservationGraph &graph,
                             EnvironmentConsistencyRequest required_scope)
        : journal_(journal), graph_(graph), required_scope_(std::move(required_scope)) {}

    /// \brief Verifies all pre-side-effect invariants and commits `dispatching`.
    /// \param key Account-scoped operation to admit.
    /// \param request Fresh account, environment, and blocker evidence.
    /// \param lease Continuously-held single-writer/fencing ownership.
    /// \return Admission status and a permit for a future internal backend call.
    /// \note This method never calls or authorizes a public `order_send` API.
    DispatchAdmissionResult admit(const OperationKey &key,
                                  const DispatchAdmissionRequest &request,
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
        if (!request.environment_proof || !request.environment_proof->valid())
            return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};
        if (request.environment_proof->account() != key.account)
            return {DispatchAdmissionStatus::account_mismatch, std::nullopt};
        if (!request.environment_proof->covers(required_scope_))
            return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};
        if (request.environment_proof->graph_instance_id() != graph_.instance_id() ||
            request.environment_proof->last_graph_revision() != graph_.revision())
            return {DispatchAdmissionStatus::environment_not_ready, std::nullopt};

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

private:
    OperationJournal &journal_;
    const ObservationGraph &graph_;
    EnvironmentConsistencyRequest required_scope_;
};

} // namespace mt5bridge

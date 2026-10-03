#pragma once

/// \file dispatch/journal.hpp
/// \brief Defines the durable operation journal and pre-side-effect admission barrier.

#include "journal_store.hpp"
#include "lease.hpp"

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

namespace mt5bridge::dispatch {
class ManagedTradeOwner;
}

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

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

/// \brief Tests whether a descriptor contains broker identities still unknown at dispatch.
/// \param descriptor Durable post-dispatch evidence contract.
/// \return True when at least one predicate needs a result-derived binding.
inline bool descriptor_requires_result_bindings(const ReconciliationDescriptor &descriptor) {
    return std::any_of(descriptor.predicates.begin(), descriptor.predicates.end(),
                       [](const ReconciliationPredicate &predicate) {
                           return predicate.ticket == 0;
                       });
}

/// \brief Builds the effective observation scope for one dispatch descriptor.
/// \param descriptor Durable predicates whose evidence must be fresh.
/// \param caller_scope Caller-requested admission scope.
/// \return Union scope, or empty when either input is malformed.
inline std::optional<EnvironmentConsistencyRequest> effective_dispatch_scope(
    const ReconciliationDescriptor &descriptor,
    EnvironmentConsistencyRequest caller_scope) {
    if (!descriptor.valid() || !caller_scope.valid())
        return std::nullopt;

    const auto extend_window = [](std::optional<ObservationWindow> *target,
                                  const std::optional<ObservationWindow> &source) {
        if (!source)
            return;
        if (!*target) {
            *target = *source;
            return;
        }
        target->emplace(ObservationWindow{
            (std::min)((*target)->from_msc, source->from_msc),
            (std::max)((*target)->to_msc, source->to_msc)});
    };

    for (const auto &predicate : descriptor.predicates) {
        switch (predicate.kind) {
        case ReconciliationPredicateKind::active_order_present:
        case ReconciliationPredicateKind::active_order_absent:
            caller_scope.require_active_orders = true;
            break;
        case ReconciliationPredicateKind::position_present:
        case ReconciliationPredicateKind::position_absent:
            caller_scope.require_positions = true;
            break;
        case ReconciliationPredicateKind::history_order_present:
        case ReconciliationPredicateKind::history_order_absent:
            if (!predicate.history_window)
                return std::nullopt;
            extend_window(&caller_scope.history_orders_window, predicate.history_window);
            break;
        case ReconciliationPredicateKind::history_deal_present:
        case ReconciliationPredicateKind::history_deal_absent:
            if (!predicate.history_window)
                return std::nullopt;
            extend_window(&caller_scope.history_deals_window, predicate.history_window);
            break;
        }
    }
    return caller_scope.valid() ? std::optional<EnvironmentConsistencyRequest>(
                                     std::move(caller_scope))
                                : std::nullopt;
}

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

/// \class JournalRecoveryResult
/// \brief Returns an all-or-nothing owner-loop recovery result.
class JournalRecoveryResult {
public:
    /// \brief Tests whether the owner cache was replaced by a complete scan.
    /// \return True only when the scan was accepted and complete.
    bool accepted() const {
        return status_ == JournalMutationStatus::accepted && complete_scan_;
    }

    /// \brief Returns the durable scan status.
    /// \return Accepted, invalid, or storage-error status.
    JournalMutationStatus status() const { return status_; }

    /// \brief Returns the records from an accepted complete scan.
    /// \return Read-only durable record set owned by this result.
    const std::vector<OperationRecord> &records() const { return records_; }

private:
    JournalRecoveryResult(JournalMutationStatus status,
                          std::vector<OperationRecord> records,
                          bool complete_scan)
        : status_(status), records_(std::move(records)), complete_scan_(complete_scan) {}

    JournalMutationStatus status_ = JournalMutationStatus::invalid_record;
    std::vector<OperationRecord> records_;
    bool complete_scan_ = false;

    friend class OperationJournal;
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
            return JournalRecoveryResult{JournalMutationStatus::invalid_record, {}, false};
        if (scanned.status == StoreScanStatus::io_error)
            return JournalRecoveryResult{JournalMutationStatus::storage_error, {}, false};

        std::map<OperationKey, OperationRecord> staged;
        for (const auto &record : scanned.records) {
            if (!record.key.valid() || !record.valid() ||
                !staged.emplace(record.key, record).second)
                return JournalRecoveryResult{JournalMutationStatus::invalid_record, {}, false};
        }
        std::vector<OperationRecord> recovered;
        recovered.reserve(staged.size());
        for (const auto &entry : staged)
            recovered.push_back(entry.second);
        records_.swap(staged);
        return JournalRecoveryResult{JournalMutationStatus::accepted,
                                     std::move(recovered), true};
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
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (it->second.reconciliation_descriptor &&
            descriptor_requires_result_bindings(*it->second.reconciliation_descriptor))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        return persist_result_with_bindings(key, std::move(result_payload), {});
    }

    /// \brief Durably advances the managed operation lifecycle state.
    /// \param key Account and managed-operation identity.
    /// \param next_state Operation state requested by the owner loop.
    /// \return Mutation status and the committed next record.
    JournalMutationResult transition_operation(const OperationKey &key,
                                               OperationState next_state) {
        return transition_operation(key, next_state, std::nullopt);
    }

private:
    JournalMutationResult transition_operation(
        const OperationKey &key, OperationState next_state,
        std::optional<std::uint64_t> settled_volume) {
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
        const bool fill_state = next_state == OperationState::partially_filled ||
                                next_state == OperationState::filled;
        if (settled_volume && (!fill_state || *settled_volume == 0))
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (fill_state && it->second.reconciliation_descriptor &&
            it->second.reconciliation_descriptor->requested_volume != 0 &&
            !settled_volume)
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        candidate.operation_state = next_state;
        if (settled_volume)
            candidate.settled_volume = *settled_volume;
        else if (!fill_state)
            candidate.settled_volume = 0;
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

public:
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
    /// \brief Atomically persists a deterministic broker rejection.
    /// \param key Account and managed-operation identity.
    /// \param result_payload Complete opaque broker result bytes.
    /// \return Mutation status and the terminal rejected record.
    /// \note A deterministic no-effect rejection does not need broker ticket
    ///       bindings because no post-dispatch identity can have been created.
    JournalMutationResult persist_rejected_result(
        const OperationKey &key, std::vector<std::uint8_t> result_payload) {
        const auto it = records_.find(key);
        if (it == records_.end())
            return {JournalMutationStatus::not_found, std::nullopt};
        if (it->second.journal_state != JournalState::dispatching ||
            it->second.operation_state != OperationState::submitting ||
            result_payload.empty())
            return {JournalMutationStatus::invalid_transition, std::nullopt};
        if (it->second.revision == (std::numeric_limits<std::uint64_t>::max)())
            return {JournalMutationStatus::invalid_record, std::nullopt};

        OperationRecord candidate = it->second;
        candidate.journal_state = JournalState::result_persisted;
        candidate.operation_state = OperationState::rejected;
        candidate.result_payload = std::move(result_payload);
        candidate.reconciliation_bindings.clear();
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
        if (it->second.reconciliation_descriptor &&
            !descriptor_bindings_complete(*it->second.reconciliation_descriptor, bindings))
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
    friend class dispatch::ManagedTradeOwner;
};

} // namespace mt5bridge

// Kept for source compatibility: older consumers obtained the admission
// contract from journal.hpp before it had its own focused header.
#include "admission.hpp"

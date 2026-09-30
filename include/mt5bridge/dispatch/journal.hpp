#pragma once

/// \file dispatch/journal.hpp
/// \brief Defines the durable operation journal and pre-side-effect admission barrier.

#include "journal_store.hpp"

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
    /// \param required_scope Caller-requested domains and history range. The
    /// descriptor predicates are merged into this scope before proof checking.
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

private:
    OperationJournal &journal_;
    const ObservationGraph &graph_;
    EnvironmentConsistencyRequest required_scope_;
};

} // namespace mt5bridge

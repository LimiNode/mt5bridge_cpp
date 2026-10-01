#pragma once

/// \file dispatch/admission.hpp
/// \brief Defines the durable pre-side-effect dispatch admission contract.

#include "journal.hpp"

namespace mt5bridge {

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
    admitted,              ///< `dispatching` was durably committed.
    invalid_request,       ///< Identity or admission evidence is malformed.
    operation_not_found,   ///< This owner loop has not created/recovered the key.
    invalid_state,         ///< The operation is not at the pre-side-effect edge.
    environment_not_ready, ///< The bounded topology proof is not consistent.
    account_mismatch,      ///< Current/evidence account differs from operation key.
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

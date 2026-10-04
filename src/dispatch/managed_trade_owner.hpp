#pragma once

/// \file dispatch/managed_trade_owner.hpp
/// \brief Defines the private owner-loop bridge for one managed trade operation.

#include "managed_trade.hpp"
#include "one_shot_backend.hpp"

#include <mt5bridge/dispatch/admission.hpp>
#include <mt5bridge/dispatch/operation_worker.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge::dispatch {

/// \enum OwnerStepStatus
/// \brief Reports one bounded owner-loop progression step.
enum class OwnerStepStatus {
    prepared,          ///< Intent and descriptor are durably ready for admission.
    awaiting_reconciliation, ///< A durable result awaits authoritative observation.
    partially_filled,  ///< Fresh deal evidence proved a partial open slice.
    completed,          ///< The managed operation reached a terminal logical state.
    ambiguous,          ///< The outcome is non-resendable and needs fresh observation.
    invalid_request,    ///< Caller input or the managed state is malformed.
    invalid_state,      ///< No operation is eligible for this owner-loop step.
    durable_failure,    ///< A required journal mutation was not committed.
    admission_rejected, ///< The dispatch barrier did not open; transport was not called.
    execution_failed,   ///< The guarded backend did not complete its call path.
};

/// \struct ManagedTradeIntent
/// \brief Opaque request and immutable reconciliation contract for one operation.
struct ManagedTradeIntent {
    std::vector<std::uint8_t> request_payload;
    ReconciliationDescriptor reconciliation_descriptor;
};

/// \struct OwnerStepResult
/// \brief Reports durable, admission, and execution evidence from one step.
struct OwnerStepResult {
    OwnerStepStatus status = OwnerStepStatus::invalid_request;
    std::optional<OperationKey> key;
    std::optional<OperationRecord> record;
    DispatchAdmissionStatus admission_status = DispatchAdmissionStatus::invalid_request;
    runtime::OneShotExecutionStatus execution_status =
        runtime::OneShotExecutionStatus::invalid_permit;
    std::uint32_t retcode = 0;

    /// \brief Tests whether the step advanced the owner state or prepared durable intent.
    /// \return True for prepared, awaiting-evidence, completed, or ambiguous outcomes.
    bool applied() const {
        return status == OwnerStepStatus::prepared ||
               status == OwnerStepStatus::awaiting_reconciliation ||
               status == OwnerStepStatus::partially_filled ||
               status == OwnerStepStatus::completed ||
               status == OwnerStepStatus::ambiguous;
    }
};

/// \class ManagedTradeOwner
/// \brief Coordinates managed state, durable admission, and one-shot execution.
///
/// This private owner loop is the only composition seam in this slice. The
/// managed state never authorizes a broker call by itself: `execute_pending`
/// must obtain a fresh `DispatchPermit`, and only the guarded one-shot backend
/// receives that permit. Broker result payloads remain owned by the journal;
/// the owner does not accept caller-supplied settlement evidence. Authoritative
/// managed settlement is delegated to provenance-bearing reconciliation.
class ManagedTradeOwner {
public:
    /// \brief Binds one managed state to the dispatch/reconciliation seams.
    /// \param initial_state Valid private logical state to own.
    /// \param account Immutable account scope for every operation key.
    /// \param journal Durable operation journal.
    /// \param admission Durable dispatch barrier bound to the same journal/graph.
    /// \param backend Guarded one-shot backend.
    /// \param account_probe Live account reader used immediately before transport.
    /// \param lease Continuously-held single-writer lease.
    /// \param transport Private single-call broker adapter.
    ManagedTradeOwner(managed_trade::ManagedTradeState initial_state,
                      AccountKey account, OperationJournal &journal,
                      DispatchAdmissionBarrier &admission,
                      runtime::OneShotDispatchBackend &backend,
                      runtime::CurrentAccountProbe &account_probe,
                      const SingleWriterLease &lease,
                      runtime::DispatchTransport &transport);

    ManagedTradeOwner(const ManagedTradeOwner &) = delete;
    ManagedTradeOwner &operator=(const ManagedTradeOwner &) = delete;

    /// \brief Rebuilds settled OPEN exposure from durable operation records.
    /// \param initial_state Empty initialized state carrying trade bounds.
    /// \param account Account scope whose records may be replayed.
    /// \param recovery Complete journal scan recovered after a process restart.
    /// \return Reconstructed state, or empty when records are incomplete or inconsistent.
    static std::optional<managed_trade::ManagedTradeState> recover_settled_open(
        managed_trade::ManagedTradeState initial_state, const AccountKey &account,
        const JournalRecoveryResult &recovery);

    /// \brief Rebuilds OPEN, CLOSE, and CANCEL exposure from durable records.
    /// \param initial_state Empty initialized state carrying trade bounds.
    /// \param account Account scope whose records may be replayed.
    /// \param recovery Complete journal scan recovered after a process restart.
    /// \return Reconstructed state, or empty when records are incomplete or inconsistent.
    static std::optional<managed_trade::ManagedTradeState> recover_settled_trade(
        managed_trade::ManagedTradeState initial_state, const AccountKey &account,
        const JournalRecoveryResult &recovery);

    /// \brief Returns the current private logical state.
    /// \return Owner-loop state owned by this instance.
    const managed_trade::ManagedTradeState &state() const { return state_; }

    /// \brief Durably prepares one bounded open slice without calling a backend.
    /// \param volume Logical entry volume.
    /// \param intent Opaque request and reconciliation descriptor.
    /// \return Prepared, invalid, or durable-failure result.
    OwnerStepResult prepare_open(managed_trade::Volume volume,
                                 ManagedTradeIntent intent);

    /// \brief Durably prepares one bounded close slice without calling a backend.
    /// \param volume Logical exposure volume to reduce.
    /// \param intent Opaque request and reconciliation descriptor.
    /// \return Prepared, invalid, or durable-failure result.
    OwnerStepResult prepare_close(managed_trade::Volume volume,
                                  ManagedTradeIntent intent);

    /// \brief Durably prepares cancellation of the current pending entry remainder.
    /// \param intent Opaque request and reconciliation descriptor.
    /// \return Prepared, invalid, or durable-failure result.
    OwnerStepResult prepare_cancel(ManagedTradeIntent intent);

    /// \brief Admits and executes the current prepared operation exactly once.
    /// \param request Fresh account, environment, and blocker evidence.
    /// \return Admission, execution, or unresolved outcome.
    OwnerStepResult execute_pending(const DispatchAdmissionRequest &request);

    /// \brief Settles the current open slice through authoritative observation.
    /// \param worker Provenance-bearing worker bound to the current operation.
    /// \param trade_event_gap Whether the caller lost event continuity.
    /// \param deadline_expired Whether the bounded observation deadline elapsed.
    /// \return Settled, awaiting, ambiguous, or durable-failure outcome.
    OwnerStepResult settle_reconciliation(
        OperationReconciliationWorker &worker, bool trade_event_gap = false,
        bool deadline_expired = false);

    /// \brief Settles newly observed volume of a previously partial OPEN slice.
    /// \param worker Fresh worker cycle bound to the partial operation.
    /// \param trade_event_gap Whether the caller lost event continuity.
    /// \param deadline_expired Whether the bounded observation deadline elapsed.
    /// \return Updated partial/completed state, awaiting evidence, or ambiguity.
    OwnerStepResult settle_pending_remainder(
        OperationReconciliationWorker &worker, bool trade_event_gap = false,
        bool deadline_expired = false);

    /// \brief Settles one CLOSE slice through attributed exit-deal evidence.
    /// \param worker Provenance-bearing worker bound to the current operation.
    /// \param trade_event_gap Whether the caller lost event continuity.
    /// \param deadline_expired Whether the bounded observation deadline elapsed.
    /// \return Settled, awaiting, ambiguous, or durable-failure outcome.
    OwnerStepResult settle_close_reconciliation(
        OperationReconciliationWorker &worker, bool trade_event_gap = false,
        bool deadline_expired = false);

    /// \brief Settles one CANCEL slice after authoritative order absence.
    /// \param worker Provenance-bearing worker bound to the current operation.
    /// \param trade_event_gap Whether the caller lost event continuity.
    /// \param deadline_expired Whether the bounded observation deadline elapsed.
    /// \return Cancelled, awaiting, ambiguous, or durable-failure outcome.
    OwnerStepResult settle_cancel_reconciliation(
        OperationReconciliationWorker &worker, bool trade_event_gap = false,
        bool deadline_expired = false);

private:
    enum class DealDirection { entry, exit };

    /// \struct HistoryDealAttributionContext
    /// \brief Validated order identity and window for one OPEN settlement.
    struct HistoryDealAttributionContext {
        ObservationWindow requested_window;
        std::uint64_t order_ticket = 0;
        std::uint64_t position_id = 0;
    };

    /// \brief Derives the account-scoped key for the current logical slice.
    std::optional<OperationKey> current_key() const;

    /// \brief Builds a result carrying the latest durable record.
    OwnerStepResult result_for(OwnerStepStatus status,
                               std::optional<OperationKey> key = std::nullopt,
                               std::optional<OperationRecord> record = std::nullopt) const;

    /// \brief Persists a candidate operation prepared in the private state machine.
    OwnerStepResult prepare_durable_operation(
        managed_trade::ManagedTradeState candidate, OperationKind kind,
        ManagedTradeIntent intent);

    /// \brief Finds the durable OPEN record that owns the current remainder.
    std::optional<OperationRecord> current_open_record_for_cancel() const;

    /// \brief Verifies that cancellation carries active-order and OPEN-history evidence.
    /// \param descriptor Proposed cancellation descriptor.
    /// \param open_record Durable partial OPEN record owning the remainder.
    /// \return True only when the active-order identity is bound consistently.
    bool cancel_descriptor_matches_open(const ReconciliationDescriptor &descriptor,
                                        const OperationRecord &open_record) const;

    /// \brief Aggregates the OPEN identity's cumulative entry volume for CANCEL.
    /// \param cycle Confirmed cancellation observation cycle.
    /// \param worker Worker that produced the cycle and its baseline.
    /// \param open_record Durable partial OPEN record carrying history identity.
    /// \return Cumulative attributed entry volume, or empty when evidence is incomplete.
    std::optional<managed_trade::Volume> cancel_cumulative_entry_volume(
        const OperationReconciliationCycle &cycle,
        const OperationReconciliationWorker &worker,
        const OperationRecord &open_record) const;

    /// \brief Moves the durable journal record to observation-only reconciliation.
    bool mark_journal_reconciling(const OperationKey &key);

    /// \brief Mirrors the backend's durable submitting edge in a candidate state.
    bool state_to_submitting(managed_trade::ManagedTradeState *candidate) const;

    /// \brief Verifies that a worker cycle carries trusted observation provenance.
    bool has_observation_provenance(
        const OperationReconciliationCycle &cycle,
        const OperationReconciliationWorker &worker, const OperationKey &key,
        const OperationRecord &record) const;

    /// \brief Validates the authoritative history-deal attribution context.
    /// \param cycle Confirmed observation cycle carrying the graph sample.
    /// \param worker Worker that produced the cycle and its baseline.
    /// \param record Durable operation record with result-derived bindings.
    /// \return Order identity and window, or empty when attribution/provenance
    ///         is incomplete.
    std::optional<HistoryDealAttributionContext> history_deal_attribution_context(
        const OperationReconciliationCycle &cycle,
        const OperationReconciliationWorker &worker,
        const OperationRecord &record, DealDirection direction) const;

    /// \brief Aggregates post-baseline entry deals for initial OPEN settlement.
    /// \param cycle Confirmed observation cycle carrying the graph sample.
    /// \param worker Worker that produced the cycle and its baseline.
    /// \param record Durable operation record with result-derived bindings.
    /// \return Fresh logical executed volume, or empty when attribution is
    ///         incomplete or the broker volume cannot be represented safely.
    std::optional<managed_trade::Volume> history_deal_volume(
        const OperationReconciliationCycle &cycle,
        const OperationReconciliationWorker &worker,
        const OperationRecord &record, DealDirection direction) const;

    /// \brief Aggregates cumulative attributed volume for a late OPEN or CLOSE fill.
    /// \param cycle Confirmed observation cycle carrying a complete history window.
    /// \param worker Worker that produced the cycle and its baseline.
    /// \param record Durable operation record with result-derived bindings.
    /// \return Cumulative logical entry volume, or empty when the authoritative
    ///         window or deal fields are incomplete.
    std::optional<managed_trade::Volume> history_deal_cumulative_volume(
        const OperationReconciliationCycle &cycle,
        const OperationReconciliationWorker &worker,
        const OperationRecord &record, DealDirection direction) const;

    managed_trade::ManagedTradeState state_;
    AccountKey account_;
    OperationJournal &journal_;
    DispatchAdmissionBarrier &admission_;
    runtime::OneShotDispatchBackend &backend_;
    runtime::CurrentAccountProbe &account_probe_;
    const SingleWriterLease &lease_;
    runtime::DispatchTransport &transport_;
};

} // namespace mt5bridge::dispatch

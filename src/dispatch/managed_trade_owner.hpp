#pragma once

/// \file dispatch/managed_trade_owner.hpp
/// \brief Defines the private owner-loop bridge for one managed trade operation.

#include "managed_trade.hpp"
#include "one_shot_backend.hpp"

#include <mt5bridge/dispatch/admission.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge::dispatch {

/// \enum OwnerStepStatus
/// \brief Reports one bounded owner-loop progression step.
enum class OwnerStepStatus {
    prepared,          ///< Intent and descriptor are durably ready for admission.
    awaiting_reconciliation, ///< A durable result awaits authoritative observation.
    completed,          ///< The managed operation reached a terminal logical state.
    ambiguous,          ///< The outcome is non-resendable and needs fresh observation.
    invalid_request,    ///< Caller input or the managed state is malformed.
    invalid_state,      ///< No operation is eligible for this owner-loop step.
    durable_failure,    ///< A required journal mutation was not committed.
    admission_rejected, ///< The dispatch barrier did not open; transport was not called.
    execution_failed,   ///< The guarded backend did not complete its call path.
};

/// \struct ManagedTradeIntent
/// \brief Opaque request and immutable reconciliation contract for one entry.
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

    /// \brief Returns the current private logical state.
    /// \return Owner-loop state owned by this instance.
    const managed_trade::ManagedTradeState &state() const { return state_; }

    /// \brief Durably prepares one bounded open slice without calling a backend.
    /// \param volume Logical entry volume.
    /// \param intent Opaque request and reconciliation descriptor.
    /// \return Prepared, invalid, or durable-failure result.
    OwnerStepResult prepare_open(managed_trade::Volume volume,
                                 ManagedTradeIntent intent);

    /// \brief Admits and executes the current prepared operation exactly once.
    /// \param request Fresh account, environment, and blocker evidence.
    /// \return Admission, execution, or unresolved outcome.
    OwnerStepResult execute_pending(const DispatchAdmissionRequest &request);

private:
    /// \brief Derives the account-scoped key for the current logical slice.
    std::optional<OperationKey> current_key() const;

    /// \brief Builds a result carrying the latest durable record.
    OwnerStepResult result_for(OwnerStepStatus status,
                               std::optional<OperationKey> key = std::nullopt,
                               std::optional<OperationRecord> record = std::nullopt) const;

    /// \brief Moves the durable journal record to observation-only reconciliation.
    bool mark_journal_reconciling(const OperationKey &key);

    /// \brief Mirrors the backend's durable submitting edge in a candidate state.
    bool state_to_submitting(managed_trade::ManagedTradeState *candidate) const;

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

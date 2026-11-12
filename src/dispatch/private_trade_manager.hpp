#pragma once

/// \file private_trade_manager.hpp
/// \brief Composes one caller-driven managed OPEN with durable startup and recovery.

#include "managed_trade_owner.hpp"
#include "private_owner_loop.hpp"

#include <memory>
#include <optional>

namespace mt5bridge::dispatch {

/// \enum PrivateTradeManagerStatus
/// \brief Reports a bounded manager action without granting a dispatch permit.
enum class PrivateTradeManagerStatus {
    ready,              ///< Startup reconstructed the managed state.
    stopped,            ///< Startup is required or shutdown was requested.
    blocked,            ///< Another operation or ambiguous exposure blocks progress.
    lease_lost,         ///< The account fencing capability is no longer held.
    recovery_failed,    ///< Durable recovery could not reconstruct this trade.
    invalid_state,      ///< The requested action does not match the durable phase.
    owner_result,       ///< The underlying managed owner returned an explicit result.
};

/// \struct PrivateTradeManagerResult
/// \brief Carries a manager decision and, when applicable, the owner outcome.
struct PrivateTradeManagerResult {
    PrivateTradeManagerStatus status = PrivateTradeManagerStatus::stopped;
    std::optional<OwnerStepResult> owner;
};

/// \class PrivateTradeManager
/// \brief Caller-driven composition for one account, trade ID, and OPEN frontier.
///
/// All dependencies outlive the manager. It owns no scheduler, interpreter, lease,
/// or broker transport. A prepared intent still requires the normal admission
/// barrier and one-shot backend; restart never manufactures a new permit.
/// \warning This is an internal composition seam. The caller must hold the
/// account lease, establish the fresh environment proof, and invoke it only
/// from the trade-critical runtime lane. A Python-backed transport remains
/// responsible for the GIL, module lifetime, request decoding, and symbol
/// validation; this class does not grant those capabilities or expose an ABI.
class PrivateTradeManager final {
public:
    /// \brief Binds one initially empty managed trade to existing private seams.
    /// \param initial Empty state with immutable trade bounds.
    /// \param account Account and fencing scope.
    /// \param journal Durable operation journal.
    /// \param admission Fresh-observation dispatch barrier for this journal.
    /// \param backend Guarded one-shot executor.
    /// \param account_probe Live account reader for the executor.
    /// \param lease Continuously-held account lease.
    /// \param transport Private broker adapter.
    /// \param coordinator Observation coordinator for caller-driven reconciliation.
    /// \param collection Domains and history window collected on each cycle.
    PrivateTradeManager(managed_trade::ManagedTradeState initial, AccountKey account,
                        OperationJournal &journal, DispatchAdmissionBarrier &admission,
                        runtime::OneShotDispatchBackend &backend,
                        runtime::CurrentAccountProbe &account_probe,
                        const SingleWriterLease &lease,
                        runtime::DispatchTransport &transport,
                        ObservationCoordinator &coordinator,
                        ObservationCollectionRequest collection);

    PrivateTradeManager(const PrivateTradeManager &) = delete;
    PrivateTradeManager &operator=(const PrivateTradeManager &) = delete;

    /// \brief Recovers the complete journal before constructing the managed owner.
    /// \return Ready only for an empty, settled, or non-resendable OPEN frontier.
    PrivateTradeManagerResult start();

    /// \brief Durably prepares one OPEN after the owner-loop blocker check.
    /// \param volume Bounded logical OPEN volume.
    /// \param intent Request payload and immutable observation descriptor.
    /// \return Explicit rejection or the managed owner result.
    PrivateTradeManagerResult prepare_open(managed_trade::Volume volume,
                                           ManagedTradeIntent intent);

    /// \brief Applies fresh admission and executes at most one broker send.
    /// \param request Fresh account, environment and blocker evidence.
    /// \return Explicit rejection or the managed owner result.
    PrivateTradeManagerResult execute_pending(const DispatchAdmissionRequest &request);

    /// \brief Performs one observation-only step for a dispatched OPEN.
    /// \param trade_event_gap Whether event hints lost continuity.
    /// \param deadline_expired Whether the bounded wait elapsed.
    /// \return Pending, partial, settled, ambiguous, or failed owner result.
    PrivateTradeManagerResult reconcile(bool trade_event_gap = false,
                                        bool deadline_expired = false);

    /// \brief Returns the reconstructed private state, if startup succeeded.
    /// \return Read-only state or empty before startup and after shutdown.
    const managed_trade::ManagedTradeState *state() const;

    /// \brief Stops this local coordinator without releasing its external lease.
    void shutdown();

private:
    PrivateTradeManagerResult check_action(const OperationKey &key,
                                           PrivateOwnerLoopStepStatus required);

    managed_trade::ManagedTradeState initial_;
    AccountKey account_;
    OperationJournal &journal_;
    DispatchAdmissionBarrier &admission_;
    runtime::OneShotDispatchBackend &backend_;
    runtime::CurrentAccountProbe &account_probe_;
    const SingleWriterLease &lease_;
    runtime::DispatchTransport &transport_;
    ObservationCoordinator &coordinator_;
    ObservationCollectionRequest collection_;
    PrivateOwnerLoop loop_;
    std::unique_ptr<ManagedTradeOwner> owner_;
};

} // namespace mt5bridge::dispatch

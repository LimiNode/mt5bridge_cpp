#pragma once

/// \file trade/managed_trade.hpp
/// \brief Defines the private durable logical managed-trade state machine.

#include <cstdint>

namespace mt5bridge::managed_trade {

/// \brief Logical volume unit used by the private lifecycle model.
using Volume = std::uint64_t;

/// \brief Monotonic identifier for one logical managed trade.
struct TradeId {
    std::uint64_t value = 0;

    /// \brief Tests whether the identifier is usable.
    /// \return True for every non-zero logical trade identifier.
    bool valid() const { return value != 0; }

    friend bool operator==(TradeId left, TradeId right) {
        return left.value == right.value;
    }

    friend bool operator!=(TradeId left, TradeId right) { return !(left == right); }
};

/// \enum PlanState
/// \brief Controls whether a plan may create new entry slices.
enum class PlanState { running, stopped };

/// \enum OperationKind
/// \brief Identifies the logical side effect represented by the current slice.
enum class OperationKind { none, open, close, cancel };

/// \enum OperationState
/// \brief Tracks one logical operation from intent through reconciliation.
enum class OperationState {
    idle,
    dispatching,
    submitting,
    reconciling,
    partially_filled,
    filled,
    cancelled,
    rejected,
    ambiguous,
};

/// \enum BrokerOutcome
/// \brief Records the evidence category returned by one broker attempt.
enum class BrokerOutcome { none, partial, full, rejected, unknown };

/// \enum TradeState
/// \brief Derived logical exposure state presented to the private owner loop.
enum class TradeState { pending, partially_open, open, reducing, closing, closed };

/// \enum MutationStatus
/// \brief Reports whether one logical state transition was applied.
enum class MutationStatus {
    applied,
    invalid_request,
    invalid_state,
    limit_reached,
    ambiguous,
};

/// \enum RecoveryAction
/// \brief Describes the safe owner-loop action after loading durable state.
enum class RecoveryAction {
    ready,
    reconcile,
    refresh_observation,
    terminal,
    invalid,
};

/// \struct CloseObligation
/// \brief Durable desired close state, independent of any close operation id.
struct CloseObligation {
    bool requested = false;
    bool ever_requested = false;
    bool satisfied = false;

    /// \brief Validates obligation invariants against logical exposure.
    /// \param open_volume Confirmed currently open volume.
    /// \param pending_remainder_volume Entry remainder that may still fill.
    /// \param unresolved_operation Whether an attempt can still change exposure.
    /// \return True when the obligation is internally consistent.
    bool valid(Volume open_volume, Volume pending_remainder_volume,
               bool unresolved_operation) const;
};

/// \struct ExecutionSlice
/// \brief Durable state and evidence for one logical operation attempt.
struct ExecutionSlice {
    std::uint64_t operation_id = 0;
    OperationKind kind = OperationKind::none;
    OperationState state = OperationState::idle;
    Volume requested_volume = 0;
    Volume result_volume = 0;
    BrokerOutcome broker_outcome = BrokerOutcome::none;
    std::uint64_t send_count = 0;
    std::uint64_t last_attempt_epoch = 0;

    /// \brief Tests whether one slice obeys lifecycle and send invariants.
    /// \param target_volume Maximum logical exposure of its owning trade.
    /// \return True when the slice is a valid durable record.
    bool valid(Volume target_volume) const;

    /// \brief Tests whether the operation may be replaced by a new one.
    /// \return True for idle and terminal operation states, never ambiguous.
    bool can_start_next() const;

    /// \brief Tests whether an open attempt may still create exposure.
    /// \return True while an open operation is unresolved or ambiguous.
    bool open_may_fill() const;
};

/// \struct ExecutionPlan
/// \brief Durable bounds and counters for sliced execution.
struct ExecutionPlan {
    PlanState state = PlanState::running;
    Volume target_volume = 0;
    Volume max_slice_volume = 0;
    std::uint64_t slice_count = 0;
    std::uint64_t max_operations = 0;

    /// \brief Validates plan bounds.
    /// \return True when target, slice, and operation limits are usable.
    bool valid() const;
};

/// \struct ManagedTradeState
/// \brief Complete private durable logical state for one managed trade.
struct ManagedTradeState {
    TradeId trade_id;
    ExecutionPlan plan;
    Volume open_volume = 0;
    Volume pending_remainder_volume = 0;
    CloseObligation close_obligation;
    std::uint64_t observation_epoch = 0;
    ExecutionSlice slice;

    /// \brief Creates a new running trade state with bounded execution limits.
    /// \param trade_id Logical identity that must never be reused for another trade.
    /// \param target_volume Desired total exposure in logical volume units.
    /// \param max_slice_volume Maximum volume of one entry slice.
    /// \param max_operations Bound on logical operation identifiers and slices.
    /// \return A valid initial state, or an empty result for invalid limits.
    static bool initialize(ManagedTradeState *state, TradeId trade_id,
                           Volume target_volume, Volume max_slice_volume,
                           std::uint64_t max_operations);

    /// \brief Validates all durable state and cross-field invariants.
    /// \return True when the state can be safely recovered.
    bool valid() const;

    /// \brief Derives the logical trade state from obligation and exposure.
    /// \return Current managed-trade state.
    TradeState trade_state() const;

    /// \brief Starts one bounded open slice.
    /// \param volume Requested entry volume.
    /// \return Applied, invalid, or limit status.
    MutationStatus start_open_slice(Volume volume);

    /// \brief Requests durable close intent without creating a send attempt.
    /// \return Applied or invalid state when no exposure can remain.
    MutationStatus request_close();

    /// \brief Starts a close operation for current confirmed exposure.
    /// \param volume Requested close volume.
    /// \return Applied, invalid, or ambiguous status.
    MutationStatus start_close(Volume volume);

    /// \brief Starts cancellation of an observed entry remainder.
    /// \return Applied, invalid, or ambiguous status.
    MutationStatus start_cancel();

    /// \brief Replays a cancellation whose remainder filled before cancellation was observed.
    /// \param volume Original cancellation request volume persisted in the journal.
    /// \return Applied or invalid state.
    MutationStatus replay_cancelled_after_late_fill(Volume volume);

    /// \brief Advances the durable operation to the one-shot submission edge.
    /// \return Applied or invalid state.
    MutationStatus enter_submitting();

    /// \brief Records one partial or full broker fill as reconciliation evidence.
    /// \param volume Volume reported by the broker.
    /// \return Applied or invalid state.
    MutationStatus record_fill(Volume volume);

    /// \brief Records an accepted cancel result.
    /// \return Applied or invalid state.
    MutationStatus record_cancel_accepted();

    /// \brief Records a deterministic broker rejection.
    /// \return Applied or invalid state.
    MutationStatus record_rejected();

    /// \brief Records a non-trustworthy/unknown broker outcome.
    /// \return Applied or invalid state.
    MutationStatus record_unknown();

    /// \brief Applies reconciliation evidence to logical exposure and remainder.
    /// \return Applied or invalid state.
    MutationStatus reconcile();

    /// \brief Applies a late fill observed for a pending entry remainder.
    /// \param volume Newly observed filled remainder volume.
    /// \return Applied or invalid state.
    MutationStatus observe_pending_remainder(Volume volume);

    /// \brief Applies a late entry fill while a CANCEL operation is pending.
    /// \param volume Newly observed volume from the durable OPEN frontier.
    /// \return Applied or invalid state.
    MutationStatus observe_cancel_remainder(Volume volume);

    /// \brief Applies a late exit fill to the same partial CLOSE operation.
    /// \param volume Newly observed close volume from the cumulative frontier.
    /// \return Applied or invalid state.
    MutationStatus observe_close_remainder(Volume volume);

    /// \brief Stops creation of new entry slices without discarding exposure.
    /// \return Applied or invalid state.
    MutationStatus stop_plan();

    /// \brief Replaces ambiguous exposure with a fresh authoritative snapshot.
    /// \param open_volume Authoritative open exposure.
    /// \param pending_remainder_volume Authoritative still-pending entry volume.
    /// \return Applied or invalid state.
    MutationStatus apply_fresh_snapshot(Volume open_volume,
                                        Volume pending_remainder_volume);

    /// \brief Retires an ambiguous operation after a newer observation epoch.
    /// \return Applied, invalid, or ambiguous status.
    MutationStatus acknowledge_ambiguous();

    /// \brief Determines the safe recovery action for this durable state.
    /// \return Recovery action; invalid state returns `invalid`.
    RecoveryAction recovery_action() const;
};

} // namespace mt5bridge::managed_trade

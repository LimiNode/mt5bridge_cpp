/// \file trade/managed_trade.cpp
/// \brief Implements the private durable logical managed-trade state machine.

#include "managed_trade.hpp"

#include <limits>
#include <utility>

namespace mt5bridge::managed_trade {
namespace {

bool valid_plan_state(PlanState state) {
    return state == PlanState::running || state == PlanState::stopped;
}

bool valid_operation_kind(OperationKind kind) {
    return kind == OperationKind::none || kind == OperationKind::open ||
           kind == OperationKind::close || kind == OperationKind::cancel;
}

bool valid_operation_state(OperationState state) {
    switch (state) {
    case OperationState::idle:
    case OperationState::dispatching:
    case OperationState::submitting:
    case OperationState::reconciling:
    case OperationState::partially_filled:
    case OperationState::filled:
    case OperationState::cancelled:
    case OperationState::rejected:
    case OperationState::ambiguous:
        return true;
    }
    return false;
}

bool valid_broker_outcome(BrokerOutcome outcome) {
    return outcome == BrokerOutcome::none || outcome == BrokerOutcome::partial ||
           outcome == BrokerOutcome::full || outcome == BrokerOutcome::rejected ||
           outcome == BrokerOutcome::unknown;
}

bool add_within(Volume left, Volume right, Volume limit) {
    return left <= limit && right <= limit - left;
}

bool is_active(OperationState state) {
    return state == OperationState::dispatching ||
           state == OperationState::submitting ||
           state == OperationState::reconciling;
}

bool is_terminal(OperationState state) {
    return state == OperationState::idle ||
           state == OperationState::partially_filled ||
           state == OperationState::filled ||
           state == OperationState::cancelled ||
           state == OperationState::rejected;
}

bool next_operation_id(ManagedTradeState *state) {
    if (state->slice.operation_id == (std::numeric_limits<std::uint64_t>::max)() ||
        state->slice.operation_id >= state->plan.max_operations)
        return false;
    state->slice.operation_id += 1;
    return true;
}

void reset_slice(ExecutionSlice *slice) {
    const auto next_id = slice->operation_id;
    *slice = ExecutionSlice{};
    slice->operation_id = next_id;
}

MutationStatus commit_candidate(ManagedTradeState *state,
                                ManagedTradeState candidate) {
    if (!candidate.valid())
        return MutationStatus::invalid_state;
    *state = std::move(candidate);
    return MutationStatus::applied;
}

} // namespace

bool CloseObligation::valid(Volume open_volume, Volume pending_remainder_volume,
                            bool unresolved_operation) const {
    if (requested && !ever_requested)
        return false;
    if (ever_requested && !requested)
        return false;
    if (satisfied && (!requested || open_volume != 0 || pending_remainder_volume != 0 ||
                      unresolved_operation))
        return false;
    if (requested && !satisfied && open_volume == 0 &&
        pending_remainder_volume == 0 && !unresolved_operation)
        return false;
    return true;
}

bool ExecutionSlice::valid(Volume target_volume) const {
    if (!valid_operation_kind(kind) || !valid_operation_state(state) ||
        !valid_broker_outcome(broker_outcome))
        return false;
    if (requested_volume > target_volume || result_volume > requested_volume ||
        send_count > 1)
        return false;
    if (kind == OperationKind::none && state != OperationState::idle)
        return false;
    if (state == OperationState::idle &&
        (kind != OperationKind::none || requested_volume != 0 || result_volume != 0 ||
         broker_outcome != BrokerOutcome::none || send_count != 0))
        return false;
    if (kind != OperationKind::none &&
        (operation_id == 0 || requested_volume == 0))
        return false;
    if (state == OperationState::dispatching &&
        (result_volume != 0 || broker_outcome != BrokerOutcome::none || send_count != 0))
        return false;
    if (state == OperationState::submitting &&
        (result_volume != 0 || broker_outcome != BrokerOutcome::none || send_count != 0))
        return false;
    if (state == OperationState::reconciling && send_count != 1)
        return false;
    if (state == OperationState::reconciling && broker_outcome == BrokerOutcome::none)
        return false;
    if (state == OperationState::reconciling && broker_outcome == BrokerOutcome::partial &&
        (kind != OperationKind::open && kind != OperationKind::close))
        return false;
    if (state == OperationState::reconciling && broker_outcome == BrokerOutcome::full &&
        ((kind == OperationKind::cancel && result_volume != 0) ||
         (kind != OperationKind::cancel && result_volume != requested_volume)))
        return false;
    if (state == OperationState::ambiguous && broker_outcome != BrokerOutcome::unknown)
        return false;
    if (state == OperationState::partially_filled &&
        (kind != OperationKind::open && kind != OperationKind::close ||
         broker_outcome != BrokerOutcome::partial || send_count != 1 || result_volume == 0 ||
         result_volume >= requested_volume))
        return false;
    if (state == OperationState::filled &&
        (kind == OperationKind::cancel || broker_outcome != BrokerOutcome::full ||
         send_count != 1 || result_volume != requested_volume))
        return false;
    if (state == OperationState::cancelled &&
        (kind != OperationKind::cancel || broker_outcome != BrokerOutcome::full ||
         send_count != 1 || result_volume != 0))
        return false;
    if (state == OperationState::rejected &&
        (broker_outcome != BrokerOutcome::rejected || send_count != 1 ||
         result_volume != 0))
        return false;
    if (broker_outcome == BrokerOutcome::partial &&
        (result_volume == 0 || result_volume >= requested_volume))
        return false;
    if (broker_outcome == BrokerOutcome::full && kind != OperationKind::cancel &&
        result_volume != requested_volume)
        return false;
    if ((broker_outcome == BrokerOutcome::rejected ||
         broker_outcome == BrokerOutcome::unknown) &&
        result_volume != 0)
        return false;
    if (send_count == 1 && state != OperationState::reconciling &&
        state != OperationState::partially_filled && state != OperationState::filled &&
        state != OperationState::cancelled && state != OperationState::rejected &&
        state != OperationState::ambiguous)
        return false;
    return true;
}

bool ExecutionSlice::can_start_next() const { return is_terminal(state); }

bool ExecutionSlice::open_may_fill() const {
    return kind == OperationKind::open &&
           (state == OperationState::dispatching || state == OperationState::submitting ||
            state == OperationState::reconciling || state == OperationState::ambiguous);
}

bool ExecutionPlan::valid() const {
    return valid_plan_state(state) && target_volume != 0 && max_slice_volume != 0 &&
           max_slice_volume <= target_volume && max_operations != 0 &&
           slice_count <= max_operations;
}

bool ManagedTradeState::initialize(ManagedTradeState *state, TradeId id,
                                   Volume target_volume, Volume max_slice_volume,
                                   std::uint64_t max_operations) {
    if (!state || !id.valid() || target_volume == 0 || max_slice_volume == 0 ||
        max_slice_volume > target_volume || max_operations == 0)
        return false;
    ManagedTradeState candidate;
    candidate.trade_id = id;
    candidate.plan.target_volume = target_volume;
    candidate.plan.max_slice_volume = max_slice_volume;
    candidate.plan.max_operations = max_operations;
    *state = candidate;
    return true;
}

bool ManagedTradeState::valid() const {
    if (!trade_id.valid() || !plan.valid() || open_volume > plan.target_volume ||
        pending_remainder_volume > plan.target_volume ||
        !add_within(open_volume, pending_remainder_volume, plan.target_volume) ||
        plan.slice_count > slice.operation_id ||
        ((plan.slice_count == 0) != (slice.operation_id == 0)) ||
        slice.operation_id > plan.max_operations ||
        !slice.valid(plan.target_volume) ||
        !close_obligation.valid(open_volume, pending_remainder_volume,
                                is_active(slice.state) ||
                                    slice.state == OperationState::ambiguous))
        return false;
    if (slice.last_attempt_epoch > observation_epoch)
        return false;
    if (slice.state == OperationState::ambiguous && slice.send_count != 1)
        return false;
    return true;
}

TradeState ManagedTradeState::trade_state() const {
    if (close_obligation.satisfied)
        return TradeState::closed;
    if (close_obligation.requested && slice.kind == OperationKind::close &&
        is_active(slice.state))
        return TradeState::closing;
    if (close_obligation.requested)
        return TradeState::reducing;
    if (open_volume == 0)
        return TradeState::pending;
    if (open_volume < plan.target_volume)
        return TradeState::partially_open;
    return TradeState::open;
}

MutationStatus ManagedTradeState::start_open_slice(Volume volume) {
    if (plan.state != PlanState::running || !slice.can_start_next() ||
        slice.state == OperationState::ambiguous || close_obligation.requested ||
        open_volume >= plan.target_volume || pending_remainder_volume != 0 || volume == 0 ||
        volume > plan.max_slice_volume || volume > plan.target_volume - open_volume)
        return slice.state == OperationState::ambiguous ? MutationStatus::ambiguous
                                                         : MutationStatus::invalid_state;
    if (plan.slice_count >= plan.max_operations ||
        slice.operation_id >= plan.max_operations ||
        slice.operation_id == (std::numeric_limits<std::uint64_t>::max)())
        return MutationStatus::limit_reached;

    auto candidate = *this;
    if (!next_operation_id(&candidate))
        return MutationStatus::limit_reached;
    candidate.plan.slice_count += 1;
    reset_slice(&candidate.slice);
    candidate.slice.operation_id = this->slice.operation_id + 1;
    candidate.slice.kind = OperationKind::open;
    candidate.slice.state = OperationState::dispatching;
    candidate.slice.requested_volume = volume;
    candidate.slice.last_attempt_epoch = candidate.observation_epoch;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::request_close() {
    if (close_obligation.requested || close_obligation.satisfied)
        return MutationStatus::invalid_state;
    if (open_volume == 0 && pending_remainder_volume == 0 && !slice.open_may_fill())
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.close_obligation.requested = true;
    candidate.close_obligation.ever_requested = true;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::start_close(Volume volume) {
    if (!close_obligation.requested || close_obligation.satisfied ||
        !slice.can_start_next() || open_volume == 0 || pending_remainder_volume != 0 ||
        volume == 0 || volume > open_volume)
        return slice.state == OperationState::ambiguous ? MutationStatus::ambiguous
                                                         : MutationStatus::invalid_state;
    auto candidate = *this;
    if (!next_operation_id(&candidate))
        return MutationStatus::limit_reached;
    reset_slice(&candidate.slice);
    candidate.slice.operation_id = this->slice.operation_id + 1;
    candidate.slice.kind = OperationKind::close;
    candidate.slice.state = OperationState::dispatching;
    candidate.slice.requested_volume = volume;
    candidate.slice.last_attempt_epoch = candidate.observation_epoch;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::start_cancel() {
    if (pending_remainder_volume == 0 || !slice.can_start_next())
        return slice.state == OperationState::ambiguous ? MutationStatus::ambiguous
                                                         : MutationStatus::invalid_state;
    auto candidate = *this;
    if (!next_operation_id(&candidate))
        return MutationStatus::limit_reached;
    reset_slice(&candidate.slice);
    candidate.slice.operation_id = this->slice.operation_id + 1;
    candidate.slice.kind = OperationKind::cancel;
    candidate.slice.state = OperationState::dispatching;
    candidate.slice.requested_volume = pending_remainder_volume;
    candidate.slice.last_attempt_epoch = candidate.observation_epoch;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::enter_submitting() {
    if (slice.state != OperationState::dispatching || slice.kind == OperationKind::none)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.slice.state = OperationState::submitting;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::record_fill(Volume volume) {
    if (slice.state != OperationState::submitting ||
        (slice.kind != OperationKind::open && slice.kind != OperationKind::close) ||
        slice.send_count != 0 || volume == 0 || volume > slice.requested_volume)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.slice.state = OperationState::reconciling;
    candidate.slice.result_volume = volume;
    candidate.slice.broker_outcome =
        volume == candidate.slice.requested_volume ? BrokerOutcome::full
                                                   : BrokerOutcome::partial;
    candidate.slice.send_count = 1;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::record_cancel_accepted() {
    if (slice.state != OperationState::submitting || slice.kind != OperationKind::cancel ||
        slice.send_count != 0)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.slice.state = OperationState::reconciling;
    candidate.slice.broker_outcome = BrokerOutcome::full;
    candidate.slice.send_count = 1;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::record_rejected() {
    if (slice.state != OperationState::submitting || slice.kind == OperationKind::none ||
        slice.send_count != 0)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.slice.state = OperationState::reconciling;
    candidate.slice.broker_outcome = BrokerOutcome::rejected;
    candidate.slice.send_count = 1;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::record_unknown() {
    if (slice.state != OperationState::submitting || slice.kind == OperationKind::none ||
        slice.send_count != 0)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.slice.state = OperationState::reconciling;
    candidate.slice.broker_outcome = BrokerOutcome::unknown;
    candidate.slice.send_count = 1;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::reconcile() {
    if (slice.state != OperationState::reconciling || slice.send_count != 1)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    switch (candidate.slice.kind) {
    case OperationKind::open:
        switch (candidate.slice.broker_outcome) {
        case BrokerOutcome::full:
            if (candidate.slice.result_volume != candidate.slice.requested_volume ||
                !add_within(candidate.open_volume, candidate.slice.result_volume,
                             candidate.plan.target_volume))
                return MutationStatus::invalid_state;
            candidate.open_volume += candidate.slice.result_volume;
            candidate.pending_remainder_volume = 0;
            candidate.slice.state = OperationState::filled;
            break;
        case BrokerOutcome::partial:
            if (candidate.slice.result_volume == 0 ||
                candidate.slice.result_volume >= candidate.slice.requested_volume ||
                !add_within(candidate.open_volume, candidate.slice.result_volume,
                             candidate.plan.target_volume))
                return MutationStatus::invalid_state;
            candidate.open_volume += candidate.slice.result_volume;
            candidate.pending_remainder_volume =
                candidate.slice.requested_volume - candidate.slice.result_volume;
            candidate.slice.state = OperationState::partially_filled;
            break;
        case BrokerOutcome::rejected:
            candidate.slice.state = OperationState::rejected;
            if (candidate.close_obligation.requested && candidate.open_volume == 0 &&
                candidate.pending_remainder_volume == 0)
                candidate.close_obligation.satisfied = true;
            break;
        case BrokerOutcome::unknown:
            candidate.slice.state = OperationState::ambiguous;
            break;
        case BrokerOutcome::none:
            return MutationStatus::invalid_state;
        }
        break;
    case OperationKind::close:
        switch (candidate.slice.broker_outcome) {
        case BrokerOutcome::full:
            if (candidate.slice.result_volume != candidate.slice.requested_volume ||
                candidate.slice.requested_volume > candidate.open_volume)
                return MutationStatus::invalid_state;
            candidate.open_volume -= candidate.slice.result_volume;
            candidate.slice.state = OperationState::filled;
            if (candidate.open_volume == 0 && candidate.pending_remainder_volume == 0)
                candidate.close_obligation.satisfied = true;
            break;
        case BrokerOutcome::partial:
            if (candidate.slice.result_volume == 0 ||
                candidate.slice.result_volume >= candidate.slice.requested_volume ||
                candidate.slice.result_volume > candidate.open_volume)
                return MutationStatus::invalid_state;
            candidate.open_volume -= candidate.slice.result_volume;
            candidate.slice.state = OperationState::partially_filled;
            break;
        case BrokerOutcome::rejected:
            candidate.slice.state = OperationState::rejected;
            break;
        case BrokerOutcome::unknown:
            candidate.slice.state = OperationState::ambiguous;
            break;
        case BrokerOutcome::none:
            return MutationStatus::invalid_state;
        }
        break;
    case OperationKind::cancel:
        switch (candidate.slice.broker_outcome) {
        case BrokerOutcome::full:
            candidate.pending_remainder_volume = 0;
            candidate.slice.state = OperationState::cancelled;
            if (candidate.close_obligation.requested && candidate.open_volume == 0)
                candidate.close_obligation.satisfied = true;
            break;
        case BrokerOutcome::rejected:
            candidate.slice.state = OperationState::rejected;
            break;
        case BrokerOutcome::unknown:
            candidate.slice.state = OperationState::ambiguous;
            break;
        case BrokerOutcome::partial:
        case BrokerOutcome::none:
            return MutationStatus::invalid_state;
        }
        break;
    case OperationKind::none:
        return MutationStatus::invalid_state;
    }
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::observe_pending_remainder(Volume volume) {
    if (pending_remainder_volume == 0 || volume == 0 || volume > pending_remainder_volume ||
        !add_within(open_volume, volume, plan.target_volume))
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.open_volume += volume;
    candidate.pending_remainder_volume -= volume;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::stop_plan() {
    if (plan.state != PlanState::running)
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.plan.state = PlanState::stopped;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::apply_fresh_snapshot(Volume fresh_open_volume,
                                                        Volume fresh_pending_volume) {
    if (slice.state != OperationState::ambiguous ||
        observation_epoch == (std::numeric_limits<std::uint64_t>::max)() ||
        !add_within(fresh_open_volume, fresh_pending_volume, plan.target_volume))
        return MutationStatus::invalid_state;
    auto candidate = *this;
    candidate.open_volume = fresh_open_volume;
    candidate.pending_remainder_volume = fresh_pending_volume;
    candidate.observation_epoch += 1;
    return commit_candidate(this, std::move(candidate));
}

MutationStatus ManagedTradeState::acknowledge_ambiguous() {
    if (slice.state != OperationState::ambiguous)
        return MutationStatus::invalid_state;
    if (observation_epoch <= slice.last_attempt_epoch)
        return MutationStatus::ambiguous;
    auto candidate = *this;
    const auto operation_id = candidate.slice.operation_id;
    candidate.slice = ExecutionSlice{};
    candidate.slice.operation_id = operation_id;
    if (candidate.close_obligation.requested && candidate.open_volume == 0 &&
        candidate.pending_remainder_volume == 0)
        candidate.close_obligation.satisfied = true;
    return commit_candidate(this, std::move(candidate));
}

RecoveryAction ManagedTradeState::recovery_action() const {
    if (!valid())
        return RecoveryAction::invalid;
    if (close_obligation.satisfied)
        return RecoveryAction::terminal;
    if (slice.state == OperationState::ambiguous)
        return RecoveryAction::refresh_observation;
    if (slice.state == OperationState::dispatching ||
        slice.state == OperationState::submitting ||
        slice.state == OperationState::reconciling)
        return RecoveryAction::reconcile;
    return RecoveryAction::ready;
}

} // namespace mt5bridge::managed_trade

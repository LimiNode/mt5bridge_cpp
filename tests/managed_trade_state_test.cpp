/// \file managed_trade_state_test.cpp
/// \brief Exercises the private durable managed-trade logical state machine.

#include "managed_trade.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using mt5bridge::managed_trade::BrokerOutcome;
using mt5bridge::managed_trade::ManagedTradeState;
using mt5bridge::managed_trade::MutationStatus;
using mt5bridge::managed_trade::OperationState;
using mt5bridge::managed_trade::RecoveryAction;
using mt5bridge::managed_trade::TradeId;
using mt5bridge::managed_trade::TradeState;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ManagedTradeState make_trade(std::uint64_t target = 10,
                             std::uint64_t max_slice = 10,
                             std::uint64_t max_operations = 8) {
    ManagedTradeState state;
    require(ManagedTradeState::initialize(&state, TradeId{1}, target, max_slice,
                                          max_operations),
            "trade initialization failed");
    require(state.valid(), "initial trade state is invalid");
    return state;
}

void test_full_open_and_close() {
    auto state = make_trade(5, 5);
    require(state.trade_state() == TradeState::pending, "initial trade is not pending");
    require(state.recovery_action() == RecoveryAction::ready,
            "initial trade is not recovery-ready");

    require(state.start_open_slice(5) == MutationStatus::applied,
            "full open slice was not started");
    require(state.enter_submitting() == MutationStatus::applied,
            "open did not enter submitting");
    require(state.record_fill(5) == MutationStatus::applied,
            "full open result was not recorded");
    require(state.reconcile() == MutationStatus::applied,
            "full open was not reconciled");
    require(state.open_volume == 5 && state.trade_state() == TradeState::open,
            "full open exposure is wrong");
    require(state.slice.send_count == 1 && state.slice.broker_outcome == BrokerOutcome::full,
            "full open send evidence is wrong");

    require(state.request_close() == MutationStatus::applied,
            "close obligation was not created");
    require(state.start_close(5) == MutationStatus::applied,
            "close operation was not started");
    require(state.enter_submitting() == MutationStatus::applied,
            "close did not enter submitting");
    require(state.record_fill(5) == MutationStatus::applied,
            "full close result was not recorded");
    require(state.reconcile() == MutationStatus::applied,
            "full close was not reconciled");
    require(state.close_obligation.satisfied && state.trade_state() == TradeState::closed,
            "full close did not satisfy the obligation");
    require(state.recovery_action() == RecoveryAction::terminal,
            "closed trade is not terminal on recovery");
}

void test_partial_remainder_cancel_and_close() {
    auto state = make_trade();
    require(state.start_open_slice(6) == MutationStatus::applied,
            "partial open slice was not started");
    require(state.enter_submitting() == MutationStatus::applied,
            "partial open did not enter submitting");
    require(state.record_fill(2) == MutationStatus::applied,
            "partial open result was not recorded");
    require(state.reconcile() == MutationStatus::applied,
            "partial open was not reconciled");
    require(state.open_volume == 2 && state.pending_remainder_volume == 4 &&
                state.slice.state == OperationState::partially_filled,
            "partial open remainder is wrong");
    require(state.start_open_slice(1) == MutationStatus::invalid_state,
            "new open bypassed a pending remainder");

    require(state.request_close() == MutationStatus::applied,
            "close obligation with remainder was not created");
    require(state.observe_pending_remainder(3) == MutationStatus::applied,
            "late remainder fill was not observed");
    require(state.open_volume == 5 && state.pending_remainder_volume == 1,
            "late remainder fill changed exposure incorrectly");
    require(state.start_cancel() == MutationStatus::applied,
            "remainder cancel was not started");
    require(state.enter_submitting() == MutationStatus::applied,
            "cancel did not enter submitting");
    require(state.record_cancel_accepted() == MutationStatus::applied,
            "cancel acceptance was not recorded");
    require(state.reconcile() == MutationStatus::applied,
            "cancel was not reconciled");
    require(state.pending_remainder_volume == 0 && state.open_volume == 5,
            "cancel changed confirmed exposure incorrectly");

    require(state.start_close(5) == MutationStatus::applied,
            "close after remainder cancellation was not started");
    require(state.enter_submitting() == MutationStatus::applied,
            "close after cancellation did not enter submitting");
    require(state.record_fill(5) == MutationStatus::applied,
            "close after cancellation was not recorded");
    require(state.reconcile() == MutationStatus::applied &&
                state.trade_state() == TradeState::closed,
            "close after cancellation did not settle the trade");
}

void test_close_before_fill_and_ambiguous_recovery() {
    auto state = make_trade(5, 5);
    require(state.start_open_slice(5) == MutationStatus::applied,
            "entry for close-before-fill was not started");
    require(state.request_close() == MutationStatus::applied,
            "close-before-fill obligation was not durable");
    require(state.trade_state() == TradeState::reducing,
            "close-before-fill state is not reducing");
    require(state.enter_submitting() == MutationStatus::applied,
            "entry did not enter submitting");
    require(state.record_unknown() == MutationStatus::applied,
            "unknown entry outcome was not recorded");
    require(state.reconcile() == MutationStatus::applied,
            "unknown entry outcome was not made ambiguous");
    require(state.slice.state == OperationState::ambiguous && state.slice.send_count == 1,
            "ambiguous operation lost its non-resendable evidence");
    require(state.recovery_action() == RecoveryAction::refresh_observation,
            "ambiguous operation did not require a fresh observation");
    require(state.apply_fresh_snapshot(0, 0) == MutationStatus::applied,
            "fresh zero snapshot was rejected");
    require(state.acknowledge_ambiguous() == MutationStatus::applied,
            "ambiguous operation was not retired after fresh evidence");
    require(state.close_obligation.satisfied && state.trade_state() == TradeState::closed,
            "zero snapshot did not satisfy close-before-fill");
    require(state.recovery_action() == RecoveryAction::terminal,
            "retired zero-exposure trade is not terminal");
}

void test_plan_stop_and_limits() {
    auto state = make_trade(2, 1, 1);
    require(state.stop_plan() == MutationStatus::applied, "plan did not stop");
    require(state.start_open_slice(1) == MutationStatus::invalid_state,
            "stopped plan created a new slice");

    auto limited = make_trade(2, 1, 1);
    require(limited.start_open_slice(1) == MutationStatus::applied,
            "limited plan did not start first slice");
    require(limited.enter_submitting() == MutationStatus::applied,
            "limited slice did not enter submitting");
    require(limited.record_fill(1) == MutationStatus::applied,
            "limited fill was not recorded");
    require(limited.reconcile() == MutationStatus::applied,
            "limited fill was not reconciled");
    require(limited.start_open_slice(1) == MutationStatus::limit_reached,
            "slice limit was not enforced");

    auto empty = make_trade();
    require(empty.request_close() == MutationStatus::invalid_state,
            "zero-exposure idle trade became closable");
}

} // namespace

int main() {
    test_full_open_and_close();
    test_partial_remainder_cancel_and_close();
    test_close_before_fill_and_ambiguous_recovery();
    test_plan_stop_and_limits();
    return 0;
}

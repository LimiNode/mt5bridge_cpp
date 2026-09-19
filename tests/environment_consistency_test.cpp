/// \file environment_consistency_test.cpp
/// \brief Exercises bounded cross-view environment consistency checks.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kOrderFields =
    MT5BRIDGE_ORDER_KNOWN_TICKET | MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
constexpr std::uint64_t kPositionFields =
    MT5BRIDGE_POSITION_KNOWN_TICKET | MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;
constexpr std::uint64_t kDealFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TIME;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account(std::uint64_t login = 42) {
    return {"Demo-Trade", login};
}

Mt5OrderSnapshot order(std::uint64_t ticket, std::uint64_t position_id) {
    Mt5OrderSnapshot value{};
    value.ticket = ticket;
    value.position_id = position_id;
    value.known_fields = kOrderFields;
    return value;
}

Mt5PositionSnapshot position(std::uint64_t ticket, std::uint64_t identifier) {
    Mt5PositionSnapshot value{};
    value.ticket = ticket;
    value.identifier = identifier;
    value.known_fields = kPositionFields;
    return value;
}

Mt5HistoryOrderSnapshot history_order(std::uint64_t ticket,
                                      std::uint64_t position_id) {
    auto value = order(ticket, position_id);
    value.time_done_msc = 1500;
    value.known_fields |= MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
    return value;
}

Mt5DealSnapshot deal(std::uint64_t ticket, std::uint64_t order_ticket,
                     std::uint64_t position_id) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_id;
    value.time_msc = 1500;
    value.known_fields = kDealFields;
    return value;
}

mt5bridge::ObservationBatch active_batch(
    const mt5bridge::AccountKey &key, std::vector<Mt5OrderSnapshot> orders,
    std::vector<Mt5PositionSnapshot> positions) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    batch.active_orders = std::move(orders);
    batch.positions = std::move(positions);
    return batch;
}

mt5bridge::ObservationBatch complete_batch(
    const mt5bridge::AccountKey &key, std::vector<Mt5OrderSnapshot> orders,
    std::vector<Mt5PositionSnapshot> positions,
    std::vector<Mt5HistoryOrderSnapshot> history_orders,
    std::vector<Mt5DealSnapshot> history_deals) {
    mt5bridge::ObservationBatch batch =
        active_batch(key, std::move(orders), std::move(positions));
    batch.observed_domains = batch.observed_domains |
                             mt5bridge::ObservationDomain::history_orders |
                             mt5bridge::ObservationDomain::history_deals;
    batch.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
    batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    batch.history_orders = std::move(history_orders);
    batch.history_deals = std::move(history_deals);
    return batch;
}

} // namespace

/// \brief Runs environment consistency contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = account();
        mt5bridge::EnvironmentConsistencyRequest request;

        const auto stable_first = active_batch(key, {order(20, 700)}, {position(500, 700)});
        const auto stable_second = stable_first;
        auto result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_first, stable_second}, request);
        require(result.consistent() &&
                    result.state == mt5bridge::EnvironmentConsistencyState::consistent,
                "equal coherent observations were not accepted");

        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate({stable_first}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation,
                "one coherent observation was treated as a stable environment");

        const auto missing_position = active_batch(key, {order(20, 700)}, {});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {missing_position, missing_position}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation &&
                    result.unresolved_links == 1,
                "missing active-order position link was not provisional");

        auto close_by = order(25, 0);
        close_by.position_by_id = 900;
        close_by.known_fields |= MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID;
        const auto missing_close_by_position =
            active_batch(key, {close_by}, {position(500, 700)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {missing_close_by_position, missing_close_by_position}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation &&
                    result.unresolved_links == 1,
                "missing ORDER_POSITION_BY_ID link was not provisional");

        mt5bridge::EnvironmentConsistencyRequest full_request = request;
        full_request.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
        full_request.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        const auto mismatch = complete_batch(
            key, {order(20, 700)}, {position(500, 700)}, {history_order(20, 700)},
            {deal(30, 20, 701)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate({mismatch, mismatch},
                                                                    full_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::cross_view_mismatch &&
                    result.contradictory_links != 0,
                "contradictory order/deal links were not rejected");

        const auto foreign = active_batch(account(43), {order(20, 700)}, {position(500, 700)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate({stable_first, foreign},
                                                                    request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::account_changed,
                "account change between collections was not rejected");

        const auto changed = active_batch(key, {order(21, 700)}, {position(500, 700)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate({stable_first, changed},
                                                                    request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::unstable_environment,
                "changing coherent observations were not bounded as unstable");

        auto confirmation_request = full_request;
        confirmation_request.max_observations = 3;
        const auto deal_before_order = complete_batch(
            key, {}, {position(500, 700)}, {}, {deal(30, 20, 700)});
        const auto order_after_deal = complete_batch(
            key, {}, {position(500, 700)}, {history_order(20, 700)},
            {deal(30, 20, 700)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {deal_before_order, order_after_deal}, confirmation_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation,
                "deal-before-order publication was treated as a contradiction");
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {deal_before_order, order_after_deal, order_after_deal}, confirmation_request);
        require(result.consistent(),
                "stable confirmation after delayed history order was not accepted");

        // A closed physical position may legitimately be absent while its deal
        // remains visible; that is not a cross-view contradiction.
        const auto closed_position = complete_batch(
            key, {}, {}, {history_order(20, 0)}, {deal(30, 20, 700)});
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {closed_position, closed_position}, full_request);
        require(result.consistent(),
                "closed-position deal was incorrectly treated as missing evidence");

        auto invalid_request = request;
        invalid_request.require_active_orders = false;
        invalid_request.require_positions = false;
        require(mt5bridge::EnvironmentConsistencyPolicy::evaluate({}, invalid_request).state ==
                    mt5bridge::EnvironmentConsistencyState::invalid_request,
                "empty policy request was not rejected");

        auto malformed = stable_first;
        malformed.active_orders[0].known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET;
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate({malformed, malformed},
                                                                    request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence,
                "incomplete graph fields were accepted by the policy");

        std::cout << "environment consistency checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "environment consistency checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

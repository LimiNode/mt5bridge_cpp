/// \file reconciliation_graph_test.cpp
/// \brief Exercises account scoping and identity links in the observation graph.

#include <mt5bridge.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

/// \brief Fails the test with an actionable message.
/// \param condition Expected condition.
/// \param message Failure description.
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// \brief Builds a minimal account identity.
/// \param server MT5 server name.
/// \param login MT5 login.
/// \return Account key for the fake observation.
mt5bridge::AccountKey account(const char *server, std::uint64_t login) {
    return {server, login};
}

/// \brief Builds one graph-relevant order snapshot.
/// \param ticket MT5 order ticket.
/// \param position_id Linked position identifier.
/// \return Minimal typed order evidence.
Mt5OrderSnapshot order(std::uint64_t ticket, std::uint64_t position_id) {
    Mt5OrderSnapshot value{};
    value.ticket = ticket;
    value.position_id = position_id;
    return value;
}

/// \brief Builds one graph-relevant position snapshot.
/// \param ticket MT5 position ticket.
/// \param identifier MT5 position identifier.
/// \return Minimal typed position evidence.
Mt5PositionSnapshot position(std::uint64_t ticket, std::uint64_t identifier) {
    Mt5PositionSnapshot value{};
    value.ticket = ticket;
    value.identifier = identifier;
    return value;
}

/// \brief Builds one graph-relevant deal snapshot.
/// \param ticket MT5 deal ticket.
/// \param order_ticket Linked order ticket.
/// \param position_id Linked position identifier.
/// \return Minimal typed deal evidence.
Mt5DealSnapshot deal(std::uint64_t ticket, std::uint64_t order_ticket,
                     std::uint64_t position_id) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_id;
    return value;
}

} // namespace

/// \brief Runs deterministic observation graph contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        Mt5AccountInfo info{};
        const char server[] = "Demo-Trade";
        std::copy(std::begin(server), std::end(server), info.server);
        info.login = 42;
        const auto key = mt5bridge::make_account_key(info);
        require(key.server == "Demo-Trade" && key.login == 42,
                "account key conversion lost identity");

        mt5bridge::ObservationGraph graph;
        mt5bridge::ObservationBatch batch;
        batch.account = key;
        batch.active_orders = {order(20, 700)};
        batch.positions = {position(500, 700)};
        batch.history_orders = {order(10, 700)};
        batch.history_deals = {deal(31, 20, 700), deal(30, 10, 700)};

        const auto first = graph.apply(batch);
        require(first.accepted() && first.revision == 1 && graph.bound(),
                "first observation was not accepted");
        require(graph.active_orders().size() == 1 && graph.positions().size() == 1 &&
                    graph.history_orders().size() == 1 && graph.history_deals().size() == 2,
                "observation counts are incorrect");

        const auto linked_orders = graph.orders_for_position(700);
        require(linked_orders.size() == 2 && linked_orders[0].ticket == 10 &&
                    linked_orders[1].ticket == 20,
                "orders were not linked by position identifier");
        const auto linked_order_deals = graph.deals_for_order(10);
        require(linked_order_deals.size() == 1 && linked_order_deals[0].ticket == 30,
                "deal-to-order link is incorrect");
        const auto linked_position_deals = graph.deals_for_position(700);
        require(linked_position_deals.size() == 2 && linked_position_deals[0].ticket == 30 &&
                    linked_position_deals[1].ticket == 31,
                "deal-to-position links are not deterministic");
        require(graph.positions_for_identifier(700).size() == 1,
                "position identifier lookup is incorrect");

        mt5bridge::ObservationBatch update;
        update.account = key;
        update.history_deals = {deal(30, 10, 700)};
        const auto second = graph.apply(update);
        require(second.accepted() && second.revision == 2 && graph.history_deals().size() == 2,
                "same-ticket observation did not upsert at a new revision");

        mt5bridge::ObservationBatch foreign;
        foreign.account = account("Other-Server", 42);
        foreign.history_deals = {deal(99, 20, 700)};
        const auto mismatch = graph.apply(foreign);
        require(mismatch.status == mt5bridge::ObservationApplyStatus::account_mismatch &&
                    mismatch.revision == 2 && graph.history_deals().size() == 2,
                "foreign account evidence crossed the graph boundary");

        mt5bridge::ObservationBatch invalid;
        invalid.account = key;
        invalid.active_orders = {order(0, 700)};
        const auto rejected = graph.apply(invalid);
        require(rejected.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    rejected.revision == 2 && graph.active_orders().size() == 1,
                "zero primary ticket was accepted");

        require(graph.clear_evidence() == 3 && graph.bound() && graph.history_deals().empty(),
                "clear did not retain account scope or advance revision");
        std::cout << "observation graph checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "observation graph checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

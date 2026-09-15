/// \file reconciliation_graph_test.cpp
/// \brief Exercises account scoping and identity links in the observation graph.

#include <mt5bridge.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

constexpr std::uint64_t kOrderGraphFields =
    MT5BRIDGE_ORDER_KNOWN_TICKET | MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
constexpr std::uint64_t kPositionGraphFields =
    MT5BRIDGE_POSITION_KNOWN_TICKET | MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;
constexpr std::uint64_t kDealGraphFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID;

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
/// \param time_done_msc Completion time used by history coverage checks.
/// \return Minimal typed order evidence.
Mt5OrderSnapshot order(std::uint64_t ticket, std::uint64_t position_id,
                       std::int64_t time_done_msc = 1500,
                       std::uint64_t known_fields =
                           kOrderGraphFields | MT5BRIDGE_ORDER_KNOWN_TIME_DONE) {
    Mt5OrderSnapshot value{};
    value.ticket = ticket;
    value.position_id = position_id;
    value.time_done_msc = time_done_msc;
    value.known_fields = known_fields;
    return value;
}

/// \brief Builds one graph-relevant position snapshot.
/// \param ticket MT5 position ticket.
/// \param identifier MT5 position identifier.
/// \return Minimal typed position evidence.
Mt5PositionSnapshot position(std::uint64_t ticket, std::uint64_t identifier,
                             std::uint64_t known_fields = kPositionGraphFields) {
    Mt5PositionSnapshot value{};
    value.ticket = ticket;
    value.identifier = identifier;
    value.known_fields = known_fields;
    return value;
}

/// \brief Builds one graph-relevant deal snapshot.
/// \param ticket MT5 deal ticket.
/// \param order_ticket Linked order ticket.
/// \param position_id Linked position identifier.
/// \param time_msc Deal time used by history coverage checks.
/// \return Minimal typed deal evidence.
Mt5DealSnapshot deal(std::uint64_t ticket, std::uint64_t order_ticket,
                     std::uint64_t position_id, std::int64_t time_msc = 1500,
                     std::uint64_t known_fields =
                         kDealGraphFields | MT5BRIDGE_DEAL_KNOWN_TIME) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_id;
    value.time_msc = time_msc;
    value.known_fields = known_fields;
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
        info.known_fields = MT5BRIDGE_ACCOUNT_KNOWN_SERVER |
                            MT5BRIDGE_ACCOUNT_KNOWN_LOGIN;
        const auto key = mt5bridge::make_account_key(info);
        require(key.valid() && key.server == "Demo-Trade" && key.login == 42,
                "account key conversion lost identity");
        auto incomplete_info = info;
        incomplete_info.known_fields &= ~MT5BRIDGE_ACCOUNT_KNOWN_LOGIN;
        require(!mt5bridge::make_account_key(incomplete_info).valid(),
                "account key ignored missing known_fields bits");

        const auto active_domain = mt5bridge::ObservationDomain::active_orders;
        const auto position_domain = mt5bridge::ObservationDomain::positions;
        const auto history_order_domain = mt5bridge::ObservationDomain::history_orders;
        const auto history_deal_domain = mt5bridge::ObservationDomain::history_deals;
        const auto all_domains = active_domain | position_domain | history_order_domain |
                                 history_deal_domain;

        mt5bridge::ObservationGraph graph;
        mt5bridge::ObservationBatch batch;
        batch.account = key;
        batch.observed_domains = all_domains;
        batch.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
        batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        batch.active_orders = {order(20, 700)};
        batch.positions = {position(500, 700)};
        // The same ticket exists in both namespaces; the graph must preserve
        // that provenance instead of returning one ambiguous combined record.
        batch.history_orders = {order(20, 700), order(10, 700, 1600)};
        batch.history_deals = {deal(31, 20, 700, 1700), deal(30, 10, 700, 1600)};

        const auto first = graph.apply(batch);
        require(first.accepted() && first.revision == 1 && graph.bound(),
                "first observation was not accepted");
        require(graph.active_orders().size() == 1 && graph.positions().size() == 1 &&
                    graph.history_orders().size() == 2 && graph.history_deals().size() == 2,
                "observation counts are incorrect");

        const auto active_links = graph.active_orders_for_position(700);
        require(active_links.size() == 1 && active_links[0].ticket == 20,
                "active order provenance was not preserved");
        const auto history_links = graph.history_orders_for_position(700);
        require(history_links.size() == 2 && history_links[0].ticket == 10 &&
                    history_links[1].ticket == 20,
                "history order provenance or sorting is incorrect");
        const auto linked_order_deals = graph.deals_for_order(10);
        require(linked_order_deals.size() == 1 && linked_order_deals[0].ticket == 30,
                "deal-to-order link is incorrect");
        const auto linked_position_deals = graph.deals_for_position(700);
        require(linked_position_deals.size() == 2 && linked_position_deals[0].ticket == 30 &&
                    linked_position_deals[1].ticket == 31,
                "deal-to-position links are not deterministic");
        require(graph.positions_for_identifier(700).size() == 1,
                "position identifier lookup is incorrect");
        require(graph.history_orders_coverage().size() == 1 &&
                    graph.history_orders_coverage()[0].window.from_msc == 1000 &&
                    graph.history_orders_coverage()[0].window.to_msc == 2000 &&
                    graph.history_orders_coverage()[0].revision == 1,
                "history-order coverage was not recorded");
        require(graph.domain_revision(active_domain) == 1 &&
                    graph.domain_revision(position_domain) == 1 &&
                    graph.domain_revision(history_order_domain) == 1 &&
                    graph.domain_revision(history_deal_domain) == 1,
                "initial domain revisions are incorrect");

        // An observed active domain is authoritative: an empty result clears
        // it, while the omitted positions domain remains unchanged.
        mt5bridge::ObservationBatch active_refresh;
        active_refresh.account = key;
        active_refresh.observed_domains = active_domain;
        const auto second = graph.apply(active_refresh);
        require(second.accepted() && second.revision == 2 && graph.active_orders().empty() &&
                    graph.positions().size() == 1 &&
                    graph.domain_revision(active_domain) == 2 &&
                    graph.domain_revision(position_domain) == 1,
                "authoritative active refresh did not replace the namespace");

        mt5bridge::ObservationBatch position_refresh;
        position_refresh.account = key;
        position_refresh.observed_domains = position_domain;
        const auto third = graph.apply(position_refresh);
        require(third.accepted() && third.revision == 3 && graph.positions().empty() &&
                    graph.history_deals().size() == 2 &&
                    graph.domain_revision(position_domain) == 3 &&
                    graph.domain_revision(history_deal_domain) == 1,
                "authoritative position refresh changed the wrong namespace");

        // History is positive evidence: an overlapping window upserts records
        // and retains revision provenance without clearing earlier records.
        mt5bridge::ObservationBatch history_update;
        history_update.account = key;
        history_update.observed_domains = history_deal_domain;
        history_update.history_deals_window = mt5bridge::ObservationWindow{1500, 2500};
        history_update.history_deals = {deal(30, 10, 700, 1800)};
        const auto fourth = graph.apply(history_update);
        require(fourth.accepted() && fourth.revision == 4 && graph.history_deals().size() == 2 &&
                    graph.history_deals_coverage().size() == 2 &&
                    graph.history_deals_coverage()[0].window.from_msc == 1000 &&
                    graph.history_deals_coverage()[0].window.to_msc == 2000 &&
                    graph.history_deals_coverage()[0].revision == 1 &&
                    graph.history_deals_coverage()[1].window.from_msc == 1500 &&
                    graph.history_deals_coverage()[1].window.to_msc == 2500 &&
                    graph.history_deals_coverage()[1].revision == 4 &&
                    graph.domain_revision(history_deal_domain) == 4,
                "history upsert or coverage provenance is incorrect");
        require(graph.history_deals_covered({1500, 2500}, 3) &&
                    !graph.history_deals_covered({1000, 2500}, 3) &&
                    !graph.history_deals_covered({1500, 2500}, 4),
                "history coverage freshness proof is incorrect");

        mt5bridge::ObservationBatch positive_only;
        positive_only.account = key;
        positive_only.observed_domains = history_deal_domain;
        positive_only.history_deals = {deal(32, 20, 700, 0)};
        const auto fifth = graph.apply(positive_only);
        require(fifth.accepted() && fifth.revision == 5 && graph.history_deals().size() == 3 &&
                    graph.history_deals_coverage().size() == 2 &&
                    graph.domain_revision(history_deal_domain) == 5,
                "positive history evidence without coverage was rejected incorrectly");

        mt5bridge::ObservationBatch foreign;
        foreign.account = account("Other-Server", 42);
        foreign.observed_domains = history_deal_domain;
        foreign.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        foreign.history_deals = {deal(99, 20, 700)};
        const auto mismatch = graph.apply(foreign);
        require(mismatch.status == mt5bridge::ObservationApplyStatus::account_mismatch &&
                    mismatch.revision == 5 && graph.history_deals().size() == 3,
                "foreign account evidence crossed the graph boundary");

        mt5bridge::ObservationBatch invalid;
        invalid.account = key;
        invalid.observed_domains = active_domain;
        invalid.active_orders = {order(40, 700), order(40, 701)};
        const auto duplicate = graph.apply(invalid);
        require(duplicate.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    duplicate.revision == 5 && graph.active_orders().empty(),
                "duplicate active ticket was accepted");

        mt5bridge::ObservationBatch zero_ticket;
        zero_ticket.account = key;
        zero_ticket.observed_domains = active_domain;
        zero_ticket.active_orders = {order(0, 700)};
        const auto zero_primary = graph.apply(zero_ticket);
        require(zero_primary.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    zero_primary.revision == 5 && graph.active_orders().empty(),
                "zero active ticket was accepted");

        mt5bridge::ObservationBatch duplicate_history;
        duplicate_history.account = key;
        duplicate_history.observed_domains = history_deal_domain;
        duplicate_history.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        duplicate_history.history_deals = {deal(41, 10, 700), deal(41, 20, 700)};
        const auto duplicate_history_result = graph.apply(duplicate_history);
        require(duplicate_history_result.status ==
                        mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    duplicate_history_result.revision == 5 && graph.history_deals().size() == 3,
                "duplicate history ticket was accepted");

        mt5bridge::ObservationBatch missing_order_fields;
        missing_order_fields.account = key;
        missing_order_fields.observed_domains = active_domain;
        missing_order_fields.active_orders = {
            order(42, 700, 1500, MT5BRIDGE_ORDER_KNOWN_TICKET)};
        const auto missing_order = graph.apply(missing_order_fields);
        require(missing_order.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    missing_order.revision == 5,
                "order graph fields were not required");

        mt5bridge::ObservationBatch missing_position_fields;
        missing_position_fields.account = key;
        missing_position_fields.observed_domains = position_domain;
        missing_position_fields.positions = {
            position(501, 701, MT5BRIDGE_POSITION_KNOWN_TICKET)};
        const auto missing_position = graph.apply(missing_position_fields);
        require(missing_position.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    missing_position.revision == 5,
                "position graph fields were not required");

        mt5bridge::ObservationBatch missing_order_time;
        missing_order_time.account = key;
        missing_order_time.observed_domains = history_order_domain;
        missing_order_time.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
        missing_order_time.history_orders = {
            order(44, 700, 1500, kOrderGraphFields)};
        const auto missing_order_timestamp = graph.apply(missing_order_time);
        require(missing_order_timestamp.status ==
                        mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    missing_order_timestamp.revision == 5,
                "history order time field was not required for coverage");

        mt5bridge::ObservationBatch missing_deal_time;
        missing_deal_time.account = key;
        missing_deal_time.observed_domains = history_deal_domain;
        missing_deal_time.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        missing_deal_time.history_deals = {
            deal(43, 10, 700, 1500, kDealGraphFields)};
        const auto missing_deal = graph.apply(missing_deal_time);
        require(missing_deal.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    missing_deal.revision == 5,
                "history deal time field was not required for coverage");

        mt5bridge::ObservationBatch missing_domain;
        missing_domain.account = key;
        missing_domain.active_orders = {order(50, 700)};
        const auto omitted_domain = graph.apply(missing_domain);
        require(omitted_domain.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    omitted_domain.revision == 5 && graph.active_orders().empty(),
                "records without an observed domain were accepted");

        mt5bridge::ObservationBatch missing_window;
        missing_window.account = key;
        missing_window.observed_domains = history_order_domain;
        const auto missing = graph.apply(missing_window);
        require(missing.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    missing.revision == 5,
                "history observation without coverage was accepted");

        mt5bridge::ObservationBatch invalid_range;
        invalid_range.account = key;
        invalid_range.observed_domains = history_deal_domain;
        invalid_range.history_deals_window = mt5bridge::ObservationWindow{1000, 1200};
        invalid_range.history_deals = {deal(100, 10, 700, 1300)};
        const auto outside = graph.apply(invalid_range);
        require(outside.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    outside.revision == 5,
                "history record outside coverage was accepted");

        mt5bridge::ObservationBatch zero_login;
        zero_login.account = account("Demo-Trade", 0);
        zero_login.observed_domains = active_domain;
        const auto invalid_account = graph.apply(zero_login);
        require(invalid_account.status == mt5bridge::ObservationApplyStatus::invalid_account &&
                    invalid_account.revision == 5,
                "zero-login account key was accepted");

        mt5bridge::ObservationGraph invalid_scope(account("Demo-Trade", 0));
        require(!invalid_scope.bound() && invalid_scope.account_key().server.empty() &&
                    invalid_scope.account_key().login == 0,
                "invalid constructor account leaked into graph scope");

        require(graph.clear_evidence() == 6 && graph.bound() && graph.history_deals().empty() &&
                    graph.history_deals_coverage().empty() &&
                    graph.domain_revision(active_domain) == 0 &&
                    graph.domain_revision(position_domain) == 0 &&
                    graph.domain_revision(history_deal_domain) == 0 &&
                    !graph.history_deals_covered({1000, 2000}, 0),
                "clear did not retain account scope or clear coverage");
        std::cout << "observation graph checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "observation graph checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

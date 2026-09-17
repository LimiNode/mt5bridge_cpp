/// \file reconciliation_engine_test.cpp
/// \brief Exercises observation-only reconciliation predicates and freshness.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint64_t kOrderFields =
    MT5BRIDGE_ORDER_KNOWN_TICKET | MT5BRIDGE_ORDER_KNOWN_POSITION_ID |
    MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
constexpr std::uint64_t kPositionFields =
    MT5BRIDGE_POSITION_KNOWN_TICKET | MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;
constexpr std::uint64_t kDealFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TIME;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() {
    return {"Demo-Trade", 42};
}

Mt5OrderSnapshot order(std::uint64_t ticket, std::uint64_t position_id) {
    Mt5OrderSnapshot value{};
    value.ticket = ticket;
    value.position_id = position_id;
    value.time_done_msc = 1500;
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

Mt5DealSnapshot deal(std::uint64_t ticket, std::int64_t time_msc = 1500) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = 20;
    value.position_id = 700;
    value.time_msc = time_msc;
    value.known_fields = kDealFields;
    return value;
}

mt5bridge::ObservationBatch active_batch(const mt5bridge::AccountKey &key,
                                         std::vector<Mt5OrderSnapshot> orders,
                                         std::vector<Mt5PositionSnapshot> positions) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    batch.active_orders = std::move(orders);
    batch.positions = std::move(positions);
    return batch;
}

mt5bridge::ObservationBatch history_deal_batch(
    const mt5bridge::AccountKey &key, std::optional<mt5bridge::ObservationWindow> window,
    std::vector<Mt5DealSnapshot> deals) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    batch.history_deals_window = window;
    batch.history_deals = std::move(deals);
    return batch;
}

} // namespace

/// \brief Runs reconciliation predicate contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = account();
        mt5bridge::ObservationGraph graph(key);
        const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);

        require(baseline.graph_revision == 0 && baseline.account == key,
                "baseline did not capture the initial account scope");

        require(graph.apply(active_batch(key, {order(20, 700)}, {position(500, 700)}))
                        .accepted(),
                "initial active observation was rejected");

        mt5bridge::ReconciliationRequest active_request;
        active_request.baseline = baseline;
        active_request.predicates = {
            mt5bridge::require_active_order(20),
            mt5bridge::require_position(500),
        };
        const auto active_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, active_request);
        require(active_result.outcome == mt5bridge::ReconciliationOutcome::confirmed &&
                    active_result.satisfied_predicates == 2 && active_result.resolved(),
                "fresh active evidence was not confirmed");

        const auto active_baseline = mt5bridge::capture_reconciliation_baseline(graph);
        mt5bridge::ObservationBatch history_only;
        history_only.account = key;
        history_only.observed_domains = mt5bridge::ObservationDomain::history_deals;
        history_only.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        history_only.history_deals = {deal(30)};
        require(graph.apply(history_only).accepted(), "history observation was rejected");

        mt5bridge::ReconciliationRequest stale_position_request;
        stale_position_request.baseline = active_baseline;
        stale_position_request.predicates = {
            mt5bridge::require_position_absent(500),
        };
        const auto stale_position =
            mt5bridge::ReconciliationEngine::evaluate(graph, stale_position_request);
        require(stale_position.outcome == mt5bridge::ReconciliationOutcome::pending &&
                    stale_position.reason ==
                        mt5bridge::ReconciliationReason::waiting_for_observation,
                "stale position evidence was treated as fresh");
        stale_position_request.deadline_expired = true;
        require(mt5bridge::ReconciliationEngine::evaluate(graph, stale_position_request).outcome ==
                    mt5bridge::ReconciliationOutcome::pending,
                "stale position evidence became unresolved without a fresh snapshot");
        stale_position_request.deadline_expired = false;

        require(graph.apply(active_batch(key, {}, {})).accepted(),
                "authoritative disappearance observation was rejected");
        const auto closed_position =
            mt5bridge::ReconciliationEngine::evaluate(graph, stale_position_request);
        require(closed_position.outcome == mt5bridge::ReconciliationOutcome::confirmed,
                "fresh authoritative position absence was not confirmed");

        const auto history_baseline = mt5bridge::capture_reconciliation_baseline(graph);
        mt5bridge::ObservationBatch history_refresh;
        history_refresh.account = key;
        history_refresh.observed_domains = mt5bridge::ObservationDomain::history_deals;
        history_refresh.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        history_refresh.history_deals = {deal(31, 1800)};
        require(graph.apply(history_refresh).accepted(), "history refresh was rejected");

        mt5bridge::ReconciliationRequest history_present;
        history_present.baseline = history_baseline;
        history_present.predicates = {
            mt5bridge::require_history_deal(31, mt5bridge::ObservationWindow{1000, 2000}),
        };
        require(mt5bridge::ReconciliationEngine::evaluate(graph, history_present).outcome ==
                    mt5bridge::ReconciliationOutcome::confirmed,
                "fresh history deal was not confirmed");

        mt5bridge::ReconciliationRequest stale_history;
        stale_history.baseline = history_baseline;
        stale_history.predicates = {
            mt5bridge::require_history_deal(30, mt5bridge::ObservationWindow{1000, 2000}),
        };
        const auto stale_history_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, stale_history);
        require(stale_history_result.outcome == mt5bridge::ReconciliationOutcome::pending &&
                    stale_history_result.missing_predicates == 1,
                "old history ticket was reused as post-baseline evidence");
        stale_history.deadline_expired = true;
        require(mt5bridge::ReconciliationEngine::evaluate(graph, stale_history).outcome ==
                    mt5bridge::ReconciliationOutcome::not_observed,
                "stale history ticket did not become unresolved after the deadline");

        mt5bridge::ReconciliationRequest history_missing;
        history_missing.baseline = history_baseline;
        history_missing.predicates = {
            mt5bridge::require_history_deal(99, mt5bridge::ObservationWindow{1000, 2000}),
        };
        const auto missing_deal =
            mt5bridge::ReconciliationEngine::evaluate(graph, history_missing);
        require(missing_deal.outcome == mt5bridge::ReconciliationOutcome::pending &&
                    !missing_deal.resolved(),
                "fresh bounded history absence became terminal before the deadline");
        history_missing.deadline_expired = true;
        const auto missing_deal_after_deadline =
            mt5bridge::ReconciliationEngine::evaluate(graph, history_missing);
        require(missing_deal_after_deadline.outcome ==
                    mt5bridge::ReconciliationOutcome::not_observed &&
                    !missing_deal_after_deadline.resolved(),
                "bounded history absence did not become unresolved after the deadline");

        mt5bridge::ReconciliationRequest absent_deal;
        absent_deal.baseline = history_baseline;
        absent_deal.predicates = {
            mt5bridge::require_history_deal_absent(99, {1000, 2000}),
        };
        require(mt5bridge::ReconciliationEngine::evaluate(graph, absent_deal).outcome ==
                    mt5bridge::ReconciliationOutcome::confirmed,
                "history absence predicate was not confirmed");

        mt5bridge::ReconciliationRequest history_without_window;
        history_without_window.baseline = history_baseline;
        history_without_window.predicates = {mt5bridge::require_history_deal(31)};
        require(mt5bridge::ReconciliationEngine::evaluate(graph, history_without_window).outcome ==
                    mt5bridge::ReconciliationOutcome::confirmed,
                "history presence without a time window was not confirmed");

        const auto history_order_baseline = mt5bridge::capture_reconciliation_baseline(graph);
        mt5bridge::ObservationBatch history_order_batch;
        history_order_batch.account = key;
        history_order_batch.observed_domains = mt5bridge::ObservationDomain::history_orders;
        history_order_batch.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
        history_order_batch.history_orders = {order(21, 700)};
        require(graph.apply(history_order_batch).accepted(),
                "history order observation was rejected");
        mt5bridge::ReconciliationRequest history_order_present;
        history_order_present.baseline = history_order_baseline;
        history_order_present.predicates = {
            mt5bridge::require_history_order(21, mt5bridge::ObservationWindow{1000, 2000}),
        };
        require(mt5bridge::ReconciliationEngine::evaluate(graph, history_order_present).outcome ==
                    mt5bridge::ReconciliationOutcome::confirmed,
                "fresh history order was not confirmed");

        const auto unknown_time_baseline = mt5bridge::capture_reconciliation_baseline(graph);
        auto unknown_time_deal = deal(40);
        unknown_time_deal.known_fields &= ~MT5BRIDGE_DEAL_KNOWN_TIME;
        require(graph.apply(history_deal_batch(key, std::nullopt, {unknown_time_deal})).accepted(),
                "history evidence without native time was rejected");
        mt5bridge::ReconciliationRequest unknown_time;
        unknown_time.baseline = unknown_time_baseline;
        unknown_time.predicates = {
            mt5bridge::require_history_deal(40, mt5bridge::ObservationWindow{1000, 2000}),
        };
        const auto unknown_time_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, unknown_time);
        require(unknown_time_result.outcome == mt5bridge::ReconciliationOutcome::ambiguous &&
                    unknown_time_result.reason ==
                        mt5bridge::ReconciliationReason::contradictory_evidence &&
                    unknown_time_result.resolved(),
                "unknown history time did not fail closed for a bounded presence predicate");
        require(graph.apply(history_deal_batch(key, mt5bridge::ObservationWindow{1000, 2000}, {}))
                        .accepted(),
                "history coverage for unknown-time absence was rejected");
        mt5bridge::ReconciliationRequest unknown_time_absence;
        unknown_time_absence.baseline = unknown_time_baseline;
        unknown_time_absence.predicates = {
            mt5bridge::require_history_deal_absent(40, {1000, 2000}),
        };
        const auto unknown_time_absence_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, unknown_time_absence);
        require(unknown_time_absence_result.outcome ==
                    mt5bridge::ReconciliationOutcome::ambiguous,
                "unknown history time was treated as a bounded absence");

        const auto gap_baseline = mt5bridge::capture_reconciliation_baseline(graph);
        const bool coverage_fragments_accepted =
            graph.apply(history_deal_batch(key, mt5bridge::ObservationWindow{3000, 3999}, {}))
                .accepted() &&
            graph.apply(history_deal_batch(key, mt5bridge::ObservationWindow{4001, 5000}, {}))
                .accepted();
        require(coverage_fragments_accepted,
                "history coverage fragments were rejected");
        mt5bridge::ReconciliationRequest gap;
        gap.baseline = gap_baseline;
        gap.predicates = {
            mt5bridge::require_history_deal_absent(99, {3000, 5000}),
        };
        require(mt5bridge::ReconciliationEngine::evaluate(graph, gap).outcome ==
                    mt5bridge::ReconciliationOutcome::pending,
                "history coverage gap was treated as complete");

        require(graph.apply(active_batch(key, {order(20, 700)}, {})).accepted(),
                "active order refresh was rejected");
        mt5bridge::ReconciliationRequest active_absence;
        active_absence.baseline = history_baseline;
        active_absence.predicates = {
            mt5bridge::require_active_order_absent(20),
        };
        const auto active_still_present =
            mt5bridge::ReconciliationEngine::evaluate(graph, active_absence);
        require(active_still_present.outcome == mt5bridge::ReconciliationOutcome::pending &&
                    !active_still_present.resolved(),
                "active absence mismatch became terminal before the deadline");
        active_absence.deadline_expired = true;
        const auto active_still_present_after_deadline =
            mt5bridge::ReconciliationEngine::evaluate(graph, active_absence);
        require(active_still_present_after_deadline.outcome ==
                    mt5bridge::ReconciliationOutcome::not_observed &&
                    !active_still_present_after_deadline.resolved(),
                "active absence mismatch did not become unresolved after the deadline");

        mt5bridge::ReconciliationRequest event_gap;
        event_gap.baseline = mt5bridge::capture_reconciliation_baseline(graph);
        event_gap.trade_event_gap = true;
        event_gap.predicates = {mt5bridge::require_active_order(20)};
        const auto event_gap_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, event_gap);
        require(event_gap_result.outcome == mt5bridge::ReconciliationOutcome::trade_event_gap &&
                    !event_gap_result.resolved(),
                "event gap was hidden as ordinary pending state");
        event_gap.deadline_expired = true;
        const auto event_gap_after_deadline =
            mt5bridge::ReconciliationEngine::evaluate(graph, event_gap);
        require(event_gap_after_deadline.outcome ==
                    mt5bridge::ReconciliationOutcome::trade_event_gap &&
                    !event_gap_after_deadline.resolved(),
                "event gap became resolved merely because the deadline elapsed");

        auto foreign_baseline = baseline;
        foreign_baseline.account = {"Other-Server", 42};
        mt5bridge::ReconciliationRequest mismatch;
        mismatch.baseline = foreign_baseline;
        mismatch.predicates = {mt5bridge::require_active_order(20)};
        const auto mismatch_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, mismatch);
        require(mismatch_result.outcome == mt5bridge::ReconciliationOutcome::account_mismatch &&
                    mismatch_result.resolved(),
                "account mismatch was not isolated");

        mt5bridge::ReconciliationRequest invalid;
        invalid.baseline = baseline;
        invalid.predicates = {mt5bridge::require_active_order(0)};
        const auto invalid_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, invalid);
        require(invalid_result.outcome == mt5bridge::ReconciliationOutcome::ambiguous &&
                    invalid_result.reason == mt5bridge::ReconciliationReason::invalid_request,
                "invalid predicate was not fail-closed");

        mt5bridge::ObservationGraph unbound;
        mt5bridge::ReconciliationRequest invalid_unbound;
        invalid_unbound.baseline.account = key;
        invalid_unbound.predicates = {mt5bridge::require_active_order(0)};
        const auto invalid_unbound_result =
            mt5bridge::ReconciliationEngine::evaluate(unbound, invalid_unbound);
        require(invalid_unbound_result.outcome == mt5bridge::ReconciliationOutcome::ambiguous &&
                    invalid_unbound_result.reason ==
                        mt5bridge::ReconciliationReason::invalid_request,
                "invalid predicate was hidden by an unbound graph");

        mt5bridge::ReconciliationRequest invalid_baseline;
        invalid_baseline.baseline = baseline;
        invalid_baseline.baseline.active_orders_revision = baseline.graph_revision + 1;
        invalid_baseline.predicates = {mt5bridge::require_active_order(20)};
        const auto invalid_baseline_result =
            mt5bridge::ReconciliationEngine::evaluate(graph, invalid_baseline);
        require(invalid_baseline_result.outcome == mt5bridge::ReconciliationOutcome::ambiguous &&
                    invalid_baseline_result.reason ==
                        mt5bridge::ReconciliationReason::invalid_request,
                "inconsistent baseline revisions were not rejected");

        std::cout << "reconciliation worker checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "reconciliation worker checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

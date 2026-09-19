/// \file reconciliation_coordinator_test.cpp
/// \brief Exercises synchronous observation coordination and the dispatch gate.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

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

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

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

Mt5DealSnapshot deal(std::uint64_t ticket) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = 20;
    value.position_id = 700;
    value.time_msc = 1500;
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

class FakeProvider final : public mt5bridge::ObservationProvider {
public:
    explicit FakeProvider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &request) override {
        if (next_ == batches_.size())
            throw std::runtime_error("fake provider exhausted");
        ++calls;
        requests.push_back(request);
        return std::move(batches_[next_++]);
    }

    std::size_t calls = 0;
    std::vector<mt5bridge::ObservationCollectionRequest> requests;

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

} // namespace

/// \brief Runs coordinator and pre-dispatch consistency contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = account();
        mt5bridge::ObservationBatch initial = active_batch(key, {}, {});
        mt5bridge::ObservationBatch fresh =
            active_batch(key, {order(20, 700)}, {position(500, 700)});
        mt5bridge::ObservationBatch history;
        history.account = key;
        history.observed_domains = mt5bridge::ObservationDomain::history_deals;
        history.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        history.history_deals = {deal(30)};
        auto valid_history = history;

        FakeProvider provider({std::move(initial), std::move(fresh), std::move(history),
                               std::move(valid_history)});
        mt5bridge::ObservationCoordinator coordinator(provider);
        mt5bridge::ObservationCollectionRequest empty_collection;
        empty_collection.observe_active_orders = false;
        empty_collection.observe_positions = false;
        require(coordinator.refresh(empty_collection).apply.status ==
                        mt5bridge::ObservationApplyStatus::invalid_evidence &&
                        provider.calls == 0,
                "empty collection request advanced the provider or graph");
        mt5bridge::ObservationCollectionRequest collection;
        const auto initial_refresh = coordinator.refresh(collection);
        require(initial_refresh.apply.accepted() && initial_refresh.sample.has_value() &&
                    coordinator.graph().revision() == 1,
                "initial coordinator observation was rejected");

        const auto baseline = coordinator.capture_baseline();
        mt5bridge::DispatchConsistencyRequest gate_request;
        gate_request.baseline = baseline;
        auto gate = mt5bridge::DispatchConsistencyGate::evaluate(
            coordinator.graph(), gate_request);
        require(gate.state == mt5bridge::DispatchConsistencyState::waiting_for_active_orders,
                "stale active evidence was admitted before refresh");

        const auto fresh_refresh = coordinator.refresh(collection);
        require(fresh_refresh.apply.accepted() && fresh_refresh.sample.has_value() &&
                    provider.calls == 2,
                "fresh coordinator observation was rejected");
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), gate_request);
        require(gate.ready() && gate.reason == mt5bridge::ReconciliationReason::none,
                "fresh active and position evidence did not open the gate");

        mt5bridge::DispatchConsistencyRequest history_gate;
        history_gate.baseline = coordinator.capture_baseline();
        history_gate.require_active_orders = false;
        history_gate.require_positions = false;
        history_gate.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), history_gate);
        require(gate.state == mt5bridge::DispatchConsistencyState::waiting_for_history,
                "history gate ignored missing post-baseline coverage");
        const auto invalid_batch = coordinator.refresh(collection);
        require(invalid_batch.apply.status == mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    invalid_batch.apply.revision == 2 && !invalid_batch.sample &&
                    provider.calls == 3 &&
                    coordinator.graph().revision() == 2,
                "provider returned an unrequested domain and coordinator accepted it");
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), history_gate);
        require(gate.state == mt5bridge::DispatchConsistencyState::waiting_for_history,
                "rejected provider batch changed history freshness");

        mt5bridge::ObservationCollectionRequest history_collection;
        history_collection.observe_active_orders = false;
        history_collection.observe_positions = false;
        history_collection.history_deals_window = *history_gate.history_deals_window;
        require(coordinator.refresh(history_collection).apply.accepted() && provider.calls == 4 &&
                    provider.requests.back().history_deals_window.has_value() &&
                    provider.requests.back().history_deals_window->from_msc ==
                        history_collection.history_deals_window->from_msc &&
                    provider.requests.back().history_deals_window->to_msc ==
                        history_collection.history_deals_window->to_msc,
                "requested history observation was rejected");
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), history_gate);
        require(gate.ready(), "complete post-baseline history did not open the gate");

        auto event_gap = history_gate;
        event_gap.event_gap = true;
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), event_gap);
        require(gate.state == mt5bridge::DispatchConsistencyState::event_gap,
                "event gap was ignored by the gate");
        auto unresolved = history_gate;
        unresolved.unresolved_operation = true;
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), unresolved);
        require(gate.state == mt5bridge::DispatchConsistencyState::unresolved_operation,
                "unresolved operation was admitted by the gate");

        auto empty_gate = history_gate;
        empty_gate.require_active_orders = false;
        empty_gate.require_positions = false;
        empty_gate.history_deals_window.reset();
        require(mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(), empty_gate).state ==
                    mt5bridge::DispatchConsistencyState::invalid_request,
                "empty gate requirements produced vacuous ready");

        FakeProvider foreign_provider(
            {active_batch({"Other-Server", 42}, {}, {})});
        mt5bridge::ObservationCoordinator account_guard(foreign_provider, key);
        const auto foreign_batch = account_guard.refresh(collection);
        require(foreign_batch.apply.status == mt5bridge::ObservationApplyStatus::account_mismatch &&
                    foreign_batch.apply.revision == 0 && !foreign_batch.sample &&
                    account_guard.graph().revision() == 0,
                "foreign account batch crossed the coordinator graph boundary");

        mt5bridge::ReconciliationRequest absent_baseline;
        absent_baseline.predicates = {mt5bridge::require_active_order(20)};
        require(coordinator.evaluate(absent_baseline).reason ==
                    mt5bridge::ReconciliationReason::invalid_request,
                "missing optional baseline was not rejected");

        mt5bridge::ObservationGraph same_account_graph(key);
        auto foreign_graph_request = history_gate;
        foreign_graph_request.baseline = mt5bridge::capture_reconciliation_baseline(
            same_account_graph);
        gate = mt5bridge::DispatchConsistencyGate::evaluate(coordinator.graph(),
                                                            foreign_graph_request);
        require(gate.state == mt5bridge::DispatchConsistencyState::graph_mismatch,
                "same-account baseline crossed graph provenance boundary");

        mt5bridge::ObservationGraph foreign_account_graph({"Other-Server", 42});
        for (int i = 0; i < 3; ++i)
            require(foreign_account_graph.apply(active_batch({"Other-Server", 42}, {}, {}))
                            .accepted(),
                    "foreign graph setup failed");
        foreign_graph_request.baseline = coordinator.capture_baseline();
        gate = mt5bridge::DispatchConsistencyGate::evaluate(foreign_account_graph,
                                                            foreign_graph_request);
        require(gate.state == mt5bridge::DispatchConsistencyState::account_mismatch,
                "account mismatch was hidden by the gate");

        std::cout << "reconciliation coordinator checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "reconciliation coordinator checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

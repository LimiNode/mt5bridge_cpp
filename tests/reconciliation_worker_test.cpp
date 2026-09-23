/// \file reconciliation_worker_test.cpp
/// \brief Exercises caller-driven reconciliation worker cycles.

#include <mt5bridge.hpp>

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kOrderFields =
    MT5BRIDGE_ORDER_KNOWN_TICKET | MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
constexpr std::uint64_t kPositionFields =
    MT5BRIDGE_POSITION_KNOWN_TICKET | MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

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
        last_request = request;
        return std::move(batches_[next_++]);
    }

    std::size_t calls = 0;
    mt5bridge::ObservationCollectionRequest last_request;

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

} // namespace

/// \brief Runs caller-driven reconciliation worker contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = account();
        FakeProvider provider({active_batch(key, {}, {}),
                               active_batch(key, {order(20, 700)}, {position(500, 700)})});
        mt5bridge::ObservationCoordinator coordinator(provider, key);
        mt5bridge::ObservationCollectionRequest collection;
        require(coordinator.refresh(collection).apply.accepted(),
                "baseline observation was rejected");

        mt5bridge::ReconciliationRequest request;
        request.baseline = coordinator.capture_baseline();
        request.predicates = {mt5bridge::require_active_order(20),
                              mt5bridge::require_position(500)};
        mt5bridge::ReconciliationWorker worker(coordinator, collection, request);
        const auto cycle = worker.step();
        require(cycle.cycle == 1 && cycle.refresh.apply.accepted() &&
                    cycle.refresh.sample.has_value(),
                "worker did not retain the accepted refresh sample");
        require(cycle.reconciliation.outcome ==
                    mt5bridge::ReconciliationOutcome::confirmed &&
                    cycle.reconciliation.resolved() && worker.cycle_count() == 1,
                "worker did not confirm fresh operation evidence");
        require(worker.last_cycle().has_value() && worker.last_cycle()->cycle == 1,
                "worker did not retain its latest cycle");

        const auto settled = worker.step(true, true);
        require(settled.cycle == 1 && provider.calls == 2,
                "settled worker performed an unexpected second refresh");

        FakeProvider late_provider({active_batch(key, {}, {}),
                                    active_batch(key, {order(99, 701)}, {})});
        mt5bridge::ObservationCoordinator late_coordinator(late_provider, key);
        mt5bridge::ReconciliationRequest late_request;
        late_request.baseline = late_coordinator.capture_baseline();
        late_request.predicates = {mt5bridge::require_active_order(99)};
        mt5bridge::ReconciliationWorker late_worker(
            late_coordinator, collection, late_request);
        const auto not_observed = late_worker.step(false, true);
        require(not_observed.reconciliation.outcome ==
                    mt5bridge::ReconciliationOutcome::not_observed &&
                    !not_observed.settled(),
                "deadline incorrectly settled an unresolved operation");
        const auto later_confirmation = late_worker.step();
        require(later_confirmation.cycle == 2 &&
                    later_confirmation.reconciliation.outcome ==
                        mt5bridge::ReconciliationOutcome::confirmed,
                "later evidence did not resolve a not-observed operation");

        mt5bridge::ObservationBatch malformed;
        malformed.account = key;
        malformed.observed_domains = mt5bridge::ObservationDomain::history_deals;
        malformed.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        FakeProvider malformed_provider(
            {active_batch(key, {order(20, 700)}, {position(500, 700)}),
             std::move(malformed)});
        mt5bridge::ObservationCoordinator malformed_coordinator(malformed_provider, key);
        const auto malformed_baseline = malformed_coordinator.capture_baseline();
        require(malformed_coordinator.refresh(collection).apply.accepted(),
                "malformed-worker setup observation was rejected");
        mt5bridge::ReconciliationRequest malformed_request;
        malformed_request.baseline = malformed_baseline;
        malformed_request.predicates = {mt5bridge::require_active_order(20)};
        mt5bridge::ReconciliationWorker malformed_worker(
            malformed_coordinator, collection, malformed_request);
        const auto malformed_cycle = malformed_worker.step();
        require(!malformed_cycle.refresh.apply.accepted() &&
                    malformed_cycle.reconciliation.outcome ==
                        mt5bridge::ReconciliationOutcome::ambiguous &&
                    malformed_cycle.reconciliation.reason ==
                        mt5bridge::ReconciliationReason::contradictory_evidence,
                "rejected refresh reused stale graph evidence as confirmation");

        std::cout << "reconciliation worker checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "reconciliation worker checks failed: " << error.what() << '\n';
        return 1;
    }
}

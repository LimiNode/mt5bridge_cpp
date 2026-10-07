/// \file broker_reversal_reconciliation_test.cpp
/// \brief Exercises causal graph-to-broker-reversal reconciliation.

#include <mt5bridge/dispatch/broker_reversal_reconciliation.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

const mt5bridge::AccountKey kAccount{"Demo-Trade", 42};
const mt5bridge::ObservationWindow kWindow{1000, 2000};

Mt5PositionSnapshot position(std::uint64_t ticket, std::uint64_t identifier,
                             std::uint32_t type, double volume) {
    Mt5PositionSnapshot value{};
    value.ticket = ticket;
    value.identifier = identifier;
    value.type = type;
    value.volume = volume;
    value.known_fields = MT5BRIDGE_POSITION_KNOWN_TICKET |
                         MT5BRIDGE_POSITION_KNOWN_IDENTIFIER |
                         MT5BRIDGE_POSITION_KNOWN_TYPE |
                         MT5BRIDGE_POSITION_KNOWN_VOLUME |
                         MT5BRIDGE_POSITION_KNOWN_SYMBOL;
    std::strcpy(value.symbol, "EURUSD");
    return value;
}

Mt5DealSnapshot deal(std::uint64_t ticket, std::uint64_t order_ticket,
                     std::uint64_t position_id, std::uint32_t type,
                     std::uint32_t entry, double volume,
                     std::int64_t time_msc = 1500) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_id;
    value.type = type;
    value.entry = entry;
    value.volume = volume;
    value.time_msc = time_msc;
    value.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                         MT5BRIDGE_DEAL_KNOWN_TYPE |
                         MT5BRIDGE_DEAL_KNOWN_ENTRY |
                         MT5BRIDGE_DEAL_KNOWN_VOLUME |
                         MT5BRIDGE_DEAL_KNOWN_TIME |
                         MT5BRIDGE_DEAL_KNOWN_SYMBOL;
    std::strcpy(value.symbol, "EURUSD");
    return value;
}

mt5bridge::ObservationBatch positions_batch(
    std::vector<Mt5PositionSnapshot> positions) {
    mt5bridge::ObservationBatch batch;
    batch.account = kAccount;
    batch.observed_domains = mt5bridge::ObservationDomain::positions;
    batch.positions = std::move(positions);
    return batch;
}

mt5bridge::ObservationBatch post_batch(
    std::vector<Mt5PositionSnapshot> positions,
    std::vector<Mt5DealSnapshot> deals,
    mt5bridge::ObservationWindow window = kWindow) {
    auto batch = positions_batch(std::move(positions));
    batch.observed_domains = batch.observed_domains |
                             mt5bridge::ObservationDomain::history_deals;
    batch.history_deals_window = window;
    batch.history_deals = std::move(deals);
    return batch;
}

class Provider final : public mt5bridge::ObservationProvider {
public:
    explicit Provider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &) override {
        if (next_ == batches_.size())
            throw std::runtime_error("observation provider exhausted");
        return batches_[next_++];
    }

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

class MemoryStore final : public mt5bridge::DurableBrokerReversalStore {
public:
    mt5bridge::BrokerReversalCommitStatus commit(
        const mt5bridge::BrokerReversalRecord &value) override {
        if (!value.valid())
            return mt5bridge::BrokerReversalCommitStatus::invalid_record;
        if (!record_) {
            record_ = value;
            return mt5bridge::BrokerReversalCommitStatus::committed;
        }
        return *record_ == value
                   ? mt5bridge::BrokerReversalCommitStatus::already_committed
                   : mt5bridge::BrokerReversalCommitStatus::conflict;
    }

    mt5bridge::BrokerReversalLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (!record_)
            return {mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt};
        if (!(record_->key() == key))
            return {mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt};
        return {mt5bridge::BrokerReversalLoadStatus::found, record_};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        if (!record_)
            return {mt5bridge::BrokerReversalScanStatus::complete, {}};
        return {mt5bridge::BrokerReversalScanStatus::complete, {*record_}};
    }

private:
    std::optional<mt5bridge::BrokerReversalRecord> record_;
};

mt5bridge::BrokerReversalObservationRequest request() {
    mt5bridge::BrokerReversalObservationRequest value;
    value.deal_ticket = 701;
    value.margin_mode = mt5bridge::BrokerMarginMode::retail_netting;
    value.history_window = kWindow;
    value.volume = {2, 1};
    return value;
}

struct Samples {
    std::unique_ptr<Provider> provider;
    std::unique_ptr<mt5bridge::ObservationCoordinator> coordinator;
    mt5bridge::ObservationSample pre;
    mt5bridge::ObservationSample post;
};

struct TestClock {
    std::int64_t next_msc = 0;

    std::int64_t operator()() {
        next_msc += 1000;
        return next_msc;
    }
};

Samples make_samples(std::vector<Mt5PositionSnapshot> post_positions,
                     std::vector<Mt5DealSnapshot> deals,
                     mt5bridge::ObservationWindow window = kWindow) {
    auto provider = std::make_unique<Provider>(std::vector<mt5bridge::ObservationBatch>{
        positions_batch({position(100, 800, 0, 5.0)}),
        post_batch(std::move(post_positions), std::move(deals), window)});
    TestClock clock;
    auto coordinator = std::make_unique<mt5bridge::ObservationCoordinator>(
        *provider, kAccount, clock);
    mt5bridge::ObservationCollectionRequest pre_request;
    pre_request.observe_active_orders = false;
    mt5bridge::ObservationCollectionRequest post_request;
    post_request.observe_active_orders = false;
    post_request.history_deals_window = window;
    const auto pre_refresh = coordinator->refresh(pre_request);
    require(pre_refresh.sample.has_value(), "pre-reversal sample was not admitted");
    const auto post_refresh = coordinator->refresh(post_request);
    require(post_refresh.sample.has_value(), "post-reversal sample was not admitted");
    return {std::move(provider), std::move(coordinator), *pre_refresh.sample,
            *post_refresh.sample};
}

void check_commit_and_replay() {
    auto samples = make_samples({position(101, 800, 1, 1.0)},
                                 {deal(701, 702, 800, 1, 2, 6.0)});
    require(samples.pre.observed_at_msc() == 1000 &&
                samples.post.observed_at_msc() == 2000,
            "test observation clock did not bracket the samples");
    MemoryStore store;
    const auto first = mt5bridge::reconcile_broker_reversal(
        samples.coordinator->graph(), samples.pre, samples.post, request(), store);
    require(first.status == mt5bridge::BrokerReversalReconcileStatus::committed &&
                first.record && first.record->broker_close_leg.units == 500 &&
                first.record->broker_reverse_open_leg.units == 100,
            "causal reversal evidence was not committed");

    const auto replay = mt5bridge::reconcile_broker_reversal(
        samples.coordinator->graph(), samples.pre, samples.post, request(), store);
    require(replay.status ==
                mt5bridge::BrokerReversalReconcileStatus::already_committed,
            "identical causal reversal replay was not idempotent");
}

void check_fail_closed_boundaries() {
    auto samples = make_samples({position(101, 800, 1, 1.0)},
                                 {deal(701, 702, 800, 1, 0, 6.0)});
    MemoryStore store;
    const auto non_inout = mt5bridge::reconcile_broker_reversal(
        samples.coordinator->graph(), samples.pre, samples.post, request(), store);
    require(non_inout.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "non-INOUT deal crossed the runtime reversal boundary");

    auto duplicate = make_samples({position(101, 800, 1, 1.0),
                                   position(102, 800, 1, 1.0)},
                                  {deal(701, 702, 800, 1, 2, 6.0)});
    const auto ambiguous = mt5bridge::reconcile_broker_reversal(
        duplicate.coordinator->graph(), duplicate.pre, duplicate.post, request(),
        store);
    require(ambiguous.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "ambiguous position identity crossed the runtime boundary");

    auto unrelated = make_samples({position(101, 800, 1, 1.0)},
                                  {deal(701, 702, 800, 1, 2, 6.005)});
    const auto unnormalized = mt5bridge::reconcile_broker_reversal(
        unrelated.coordinator->graph(), unrelated.pre, unrelated.post, request(),
        store);
    require(unnormalized.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "non-step-normalized broker volume crossed the runtime boundary");

    const mt5bridge::ObservationWindow broad_window{0, 3000};
    auto old_deal = make_samples({position(101, 800, 1, 1.0)},
                                 {deal(701, 702, 800, 1, 2, 6.0, 500)},
                                 broad_window);
    auto old_deal_request = request();
    old_deal_request.history_window = broad_window;
    const auto before_pre_snapshot = mt5bridge::reconcile_broker_reversal(
        old_deal.coordinator->graph(), old_deal.pre, old_deal.post,
        old_deal_request, store);
    require(before_pre_snapshot.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "deal before pre-observation bound crossed the runtime boundary");

    auto late_deal = make_samples({position(101, 800, 1, 1.0)},
                                  {deal(701, 702, 800, 1, 2, 6.0, 2500)},
                                  broad_window);
    const auto after_post_snapshot = mt5bridge::reconcile_broker_reversal(
        late_deal.coordinator->graph(), late_deal.pre, late_deal.post,
        old_deal_request, store);
    require(after_post_snapshot.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "deal after post-observation bound crossed the runtime boundary");

    auto wrong_window_request = request();
    wrong_window_request.history_window = {0, 2000};
    const auto wrong_window = mt5bridge::reconcile_broker_reversal(
        unrelated.coordinator->graph(), unrelated.pre, unrelated.post,
        wrong_window_request, store);
    require(wrong_window.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "post-action history query window crossed the runtime boundary");

    const auto reversed_samples = mt5bridge::reconcile_broker_reversal(
        unrelated.coordinator->graph(), unrelated.post, unrelated.pre, request(),
        store);
    require(reversed_samples.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "reverse-ordered samples crossed the runtime boundary");

    auto other = make_samples({position(101, 800, 1, 1.0)},
                              {deal(701, 702, 800, 1, 2, 6.0)});
    const auto foreign_graph = mt5bridge::reconcile_broker_reversal(
        other.coordinator->graph(), samples.pre, samples.post, request(), store);
    require(foreign_graph.status ==
                mt5bridge::BrokerReversalReconcileStatus::invalid_observation,
            "samples from another graph crossed the runtime boundary");
}

} // namespace

/// \brief Runs causal broker-reversal integration checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_commit_and_replay();
        check_fail_closed_boundaries();
        std::cout << "broker reversal reconciliation checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "broker reversal reconciliation checks failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

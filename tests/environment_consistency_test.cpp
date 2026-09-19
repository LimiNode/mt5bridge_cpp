/// \file environment_consistency_test.cpp
/// \brief Exercises bounded cross-view environment consistency checks.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
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
                     std::uint64_t position_id, std::uint32_t entry = 0) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_id;
    value.entry = entry;
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

class FakeProvider final : public mt5bridge::ObservationProvider {
public:
    explicit FakeProvider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &) override {
        if (next_ == batches_.size())
            throw std::runtime_error("fake provider exhausted");
        return std::move(batches_[next_++]);
    }

    std::size_t size() const { return batches_.size(); }

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

mt5bridge::ObservationCollectionRequest collection_request(
    const mt5bridge::EnvironmentConsistencyRequest &request) {
    mt5bridge::ObservationCollectionRequest collection;
    collection.observe_active_orders = request.require_active_orders;
    collection.observe_positions = request.require_positions;
    collection.history_orders_window = request.history_orders_window;
    collection.history_deals_window = request.history_deals_window;
    return collection;
}

std::vector<mt5bridge::ObservationSample> samples_for(
    std::vector<mt5bridge::ObservationBatch> batches,
    const mt5bridge::EnvironmentConsistencyRequest &request) {
    FakeProvider provider(std::move(batches));
    mt5bridge::ObservationCoordinator coordinator(provider);
    const auto collection = collection_request(request);
    std::vector<mt5bridge::ObservationSample> samples;
    samples.reserve(provider.size());
    while (true) {
        try {
            const auto refresh = coordinator.refresh(collection);
            require(refresh.apply.accepted() && refresh.sample.has_value(),
                    "coordinator did not produce an accepted observation sample");
            samples.push_back(*refresh.sample);
        } catch (const std::runtime_error &error) {
            if (std::string(error.what()) == "fake provider exhausted")
                break;
            throw;
        }
    }
    return samples;
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
        const auto stable_samples = samples_for({stable_first, stable_second}, request);
        auto result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(stable_samples, request);
        require(result.consistent() &&
                    result.state == mt5bridge::EnvironmentConsistencyState::consistent,
                "equal coherent observations were not accepted");

        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_samples.front()}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation,
                "one coherent observation was treated as a stable environment");

        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_samples.front(), stable_samples.front()}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence,
                "the same accepted sample twice was treated as sequential evidence");

        const auto missing_position = active_batch(key, {order(20, 700)}, {});
        auto awaiting_request = request;
        awaiting_request.max_observations = 3;
        const auto missing_position_samples =
            samples_for({missing_position, missing_position}, awaiting_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            missing_position_samples, awaiting_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation &&
                    result.unresolved_links == 1,
                "missing active-order position link was not provisional");

        auto close_by = order(25, 0);
        close_by.position_by_id = 900;
        close_by.known_fields |= MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID;
        const auto missing_close_by_position =
            active_batch(key, {close_by}, {position(500, 700)});
        const auto close_by_samples =
            samples_for({missing_close_by_position, missing_close_by_position}, awaiting_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(close_by_samples,
                                                                    awaiting_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation &&
                    result.unresolved_links == 1,
                "missing ORDER_POSITION_BY_ID link was not provisional");

        mt5bridge::EnvironmentConsistencyRequest full_request = request;
        full_request.history_orders_window = mt5bridge::ObservationWindow{1000, 2000};
        full_request.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        const auto mismatch = complete_batch(
            key, {order(20, 700)}, {position(500, 700)}, {history_order(20, 700)},
            {deal(30, 20, 701)});
        const auto mismatch_samples = samples_for({mismatch, mismatch}, full_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(mismatch_samples,
                                                                    full_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::cross_view_mismatch &&
                    result.contradictory_links != 0,
                "contradictory order/deal links were not rejected");

        const auto foreign = active_batch(account(43), {order(20, 700)}, {position(500, 700)});
        const auto foreign_samples = samples_for({foreign, foreign}, request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_samples.front(), foreign_samples.front()}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::account_changed,
                "account change between collections was not rejected");

        const auto other_graph_samples = samples_for({stable_first, stable_second}, request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_samples.front(), other_graph_samples.front()}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence,
                "foreign graph provenance was accepted");

        const auto changed = active_batch(key, {order(21, 700)}, {position(500, 700)});
        const auto changed_samples = samples_for({changed, changed}, request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {stable_samples.front(), changed_samples.front()}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence,
                "non-consecutive graph samples were accepted as instability");

        // The same graph owner must provide consecutive revisions for a real
        // instability result.
        FakeProvider instability_provider({stable_first, changed, changed});
        mt5bridge::ObservationCoordinator instability_coordinator(instability_provider);
        const auto collection = collection_request(request);
        const auto first_refresh = instability_coordinator.refresh(collection);
        const auto second_refresh = instability_coordinator.refresh(collection);
        const auto third_refresh = instability_coordinator.refresh(collection);
        require(first_refresh.sample && second_refresh.sample && third_refresh.sample,
                "instability setup did not produce samples");
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first_refresh.sample, *second_refresh.sample}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::unstable_environment,
                "changing coherent observations were not bounded as unstable");
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first_refresh.sample, *third_refresh.sample}, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence,
                "a skipped graph revision was accepted as sequential evidence");

        auto confirmation_request = full_request;
        confirmation_request.max_observations = 3;
        const auto deal_before_order = complete_batch(
            key, {}, {position(500, 700)}, {}, {deal(30, 20, 700)});
        const auto order_after_deal = complete_batch(
            key, {}, {position(500, 700)}, {history_order(20, 700)},
            {deal(30, 20, 700)});
        const auto delayed_samples = samples_for(
            {deal_before_order, order_after_deal, order_after_deal}, confirmation_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {delayed_samples[0], delayed_samples[1]}, confirmation_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::awaiting_confirmation,
                "deal-before-order publication was treated as a contradiction");
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(delayed_samples,
                                                                    confirmation_request);
        require(result.consistent(),
                "stable confirmation after delayed history order was not accepted");

        const auto persistent_lag_samples =
            samples_for({deal_before_order, deal_before_order}, full_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(persistent_lag_samples,
                                                                    full_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::unstable_environment,
                "unresolved publication lag ignored the bounded observation budget");

        auto deals_only_request = full_request;
        deals_only_request.require_active_orders = false;
        deals_only_request.require_positions = false;
        deals_only_request.history_orders_window.reset();
        const auto deals_only = complete_batch(key, {}, {}, {}, {deal(30, 20, 700)});
        auto deals_only_batch = deals_only;
        deals_only_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deals_only_batch.history_orders_window.reset();
        const auto deals_only_samples = samples_for({deals_only_batch, deals_only_batch},
                                                    deals_only_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(deals_only_samples,
                                                                    deals_only_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence &&
                    result.insufficient_links != 0,
                "deal without any requested order evidence was treated as coherent");

        auto history_only_request = full_request;
        history_only_request.require_active_orders = false;
        history_only_request.require_positions = false;
        auto history_only_batch = complete_batch(
            key, {}, {}, {}, {deal(30, 20, 700)});
        history_only_batch.observed_domains =
            mt5bridge::ObservationDomain::history_orders |
            mt5bridge::ObservationDomain::history_deals;
        const auto history_only_samples =
            samples_for({history_only_batch, history_only_batch}, history_only_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            history_only_samples, history_only_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence &&
                    result.insufficient_links != 0,
                "history deal/order lag without active-order evidence was treated as safe");

        auto outside_window_request = full_request;
        outside_window_request.history_orders_window =
            mt5bridge::ObservationWindow{1000, 1200};
        auto outside_window = complete_batch(
            key, {}, {position(500, 700)}, {}, {deal(30, 20, 700)});
        outside_window.history_orders_window = outside_window_request.history_orders_window;
        const auto outside_samples =
            samples_for({outside_window, outside_window}, outside_window_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(outside_samples,
                                                                    outside_window_request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::insufficient_evidence &&
                    result.insufficient_links != 0,
                "deal outside order-history coverage was treated as publication lag");

        // A closed physical position may legitimately be absent while its deal
        // remains visible; that is not a cross-view contradiction.
        const auto closed_position = complete_batch(
            key, {}, {}, {history_order(20, 0)}, {deal(30, 20, 700)});
        const auto closed_samples = samples_for({closed_position, closed_position}, full_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(closed_samples, full_request);
        require(result.consistent(),
                "closed-position deal was incorrectly treated as missing evidence");

        // A pending order with a known zero position id is a valid topology.
        const auto pending = active_batch(key, {order(40, 0)}, {});
        const auto pending_samples = samples_for({pending, pending}, request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(pending_samples, request);
        require(result.consistent(), "known zero ORDER_POSITION_ID was rejected");

        // Reversal entry changes payload semantics, not identity topology.
        const auto reversal = complete_batch(
            key, {order(20, 700)}, {position(500, 700)}, {history_order(20, 700)},
            {deal(30, 20, 700, 2)}); // MT5 DEAL_ENTRY_INOUT.
        const auto reversal_samples = samples_for({reversal, reversal}, full_request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(reversal_samples,
                                                                    full_request);
        require(result.consistent(), "DEAL_ENTRY_INOUT changed topology unexpectedly");

        auto known_zero = order(20, 700);
        known_zero.known_fields |= MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID;
        const auto unknown_by_id = active_batch(key, {order(20, 700)}, {position(500, 700)});
        const auto known_zero_by_id = active_batch(key, {known_zero}, {position(500, 700)});
        const auto by_id_samples = samples_for({unknown_by_id, known_zero_by_id}, request);
        result = mt5bridge::EnvironmentConsistencyPolicy::evaluate(by_id_samples, request);
        require(result.state == mt5bridge::EnvironmentConsistencyState::unstable_environment,
                "unknown POSITION_BY_ID was treated as known zero");

        auto invalid_request = request;
        invalid_request.require_active_orders = false;
        invalid_request.require_positions = false;
        require(mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                    std::vector<mt5bridge::ObservationSample>{}, invalid_request)
                        .state == mt5bridge::EnvironmentConsistencyState::invalid_request,
                "empty policy request was not rejected");

        // History timestamp knownness is required even when the numeric zero
        // happens to fall inside the requested [0, 5000] window.
        auto timestamp_request = full_request;
        timestamp_request.history_orders_window = mt5bridge::ObservationWindow{0, 5000};
        timestamp_request.history_deals_window = mt5bridge::ObservationWindow{0, 5000};
        auto missing_order_time = complete_batch(
            key, {}, {}, {order(50, 700)}, {deal(51, 50, 700)});
        missing_order_time.history_orders_window = timestamp_request.history_orders_window;
        missing_order_time.history_deals_window = timestamp_request.history_deals_window;
        missing_order_time.history_orders[0].known_fields = kOrderFields;
        const auto malformed_order_provider =
            std::vector<mt5bridge::ObservationBatch>{missing_order_time};
        FakeProvider malformed_order_fake(malformed_order_provider);
        mt5bridge::ObservationCoordinator malformed_order_coordinator(malformed_order_fake);
        const auto malformed_order_refresh =
            malformed_order_coordinator.refresh(collection_request(timestamp_request));
        require(malformed_order_refresh.apply.status ==
                        mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    !malformed_order_refresh.sample,
                "history order without known completion time was accepted");

        auto missing_deal_time = complete_batch(
            key, {}, {}, {history_order(50, 700)}, {deal(51, 50, 700)});
        missing_deal_time.history_orders_window = timestamp_request.history_orders_window;
        missing_deal_time.history_deals_window = timestamp_request.history_deals_window;
        missing_deal_time.history_deals[0].known_fields =
            MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
            MT5BRIDGE_DEAL_KNOWN_POSITION_ID;
        FakeProvider malformed_deal_fake({missing_deal_time});
        mt5bridge::ObservationCoordinator malformed_deal_coordinator(malformed_deal_fake);
        const auto malformed_deal_refresh =
            malformed_deal_coordinator.refresh(collection_request(timestamp_request));
        require(malformed_deal_refresh.apply.status ==
                        mt5bridge::ObservationApplyStatus::invalid_evidence &&
                    !malformed_deal_refresh.sample,
                "history deal without known time was accepted");

        std::cout << "environment consistency checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "environment consistency checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

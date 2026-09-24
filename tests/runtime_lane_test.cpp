/// \file runtime_lane_test.cpp
/// \brief Tests bounded priority and starvation protection for runtime lanes.

#include "runtime_lane.hpp"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using mt5bridge::runtime::RuntimeCallLane;
using mt5bridge::runtime::RuntimeLane;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void wait_for_waiters(RuntimeLane &lane, RuntimeCallLane call_lane) {
    for (int attempt = 0; attempt < 1000 && lane.waiting(call_lane) == 0; ++attempt)
        std::this_thread::yield();
    require(lane.waiting(call_lane) != 0, "caller did not reach lane wait queue");
}

void trade_preempts_market_data() {
    RuntimeLane lane;
    auto held = lane.acquire(RuntimeCallLane::market_data);
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<RuntimeCallLane> order;
    bool market_started = false;
    bool trade_started = false;

    std::thread market([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            market_started = true;
        }
        condition.notify_all();
        auto permit = lane.acquire(RuntimeCallLane::market_data);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(RuntimeCallLane::market_data);
    });
    std::thread trade([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            trade_started = true;
        }
        condition.notify_all();
        auto permit = lane.acquire(RuntimeCallLane::trade_critical);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(RuntimeCallLane::trade_critical);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return market_started && trade_started; });
    }
    wait_for_waiters(lane, RuntimeCallLane::market_data);
    wait_for_waiters(lane, RuntimeCallLane::trade_critical);
    held = RuntimeLane::Permit{};
    market.join();
    trade.join();
    require(order.size() == 2, "both queued calls must complete");
    require(order[0] == RuntimeCallLane::trade_critical,
            "trade-critical call did not preempt waiting market data");
}

void market_data_is_not_starved() {
    RuntimeLane lane(2);
    auto held = lane.acquire(RuntimeCallLane::market_data);
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<RuntimeCallLane> order;
    bool market_started = false;
    std::size_t trade_started = 0;

    std::thread market([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            market_started = true;
        }
        condition.notify_all();
        auto permit = lane.acquire(RuntimeCallLane::market_data);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(RuntimeCallLane::market_data);
    });
    std::vector<std::thread> trades;
    for (int index = 0; index < 4; ++index) {
        trades.emplace_back([&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++trade_started;
            }
            condition.notify_all();
            auto permit = lane.acquire(RuntimeCallLane::trade_critical);
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(RuntimeCallLane::trade_critical);
        });
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return market_started && trade_started == trades.size(); });
    }
    wait_for_waiters(lane, RuntimeCallLane::market_data);
    wait_for_waiters(lane, RuntimeCallLane::trade_critical);
    held = RuntimeLane::Permit{};
    market.join();
    for (auto &thread : trades)
        thread.join();
    require(order.size() == 5, "all bounded-fair calls must complete");
    require(order[2] == RuntimeCallLane::market_data,
            "market data was starved past the configured trade burst");
}

void uncontended_trade_grants_do_not_consume_burst() {
    RuntimeLane lane(2);

    // These calls have no market waiter and therefore must not consume the
    // burst budget used to arbitrate a later contended exchange.
    {
        auto permit = lane.acquire(RuntimeCallLane::trade_critical);
    }
    {
        auto permit = lane.acquire(RuntimeCallLane::trade_critical);
    }

    auto held = lane.acquire(RuntimeCallLane::market_data);
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<RuntimeCallLane> order;
    bool market_started = false;
    bool trade_started = false;

    std::thread market([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            market_started = true;
        }
        condition.notify_all();
        auto permit = lane.acquire(RuntimeCallLane::market_data);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(RuntimeCallLane::market_data);
    });
    std::thread trade([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            trade_started = true;
        }
        condition.notify_all();
        auto permit = lane.acquire(RuntimeCallLane::trade_critical);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(RuntimeCallLane::trade_critical);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return market_started && trade_started; });
    }
    wait_for_waiters(lane, RuntimeCallLane::market_data);
    wait_for_waiters(lane, RuntimeCallLane::trade_critical);
    held = RuntimeLane::Permit{};
    market.join();
    trade.join();

    require(order.size() == 2, "both contended calls must complete");
    require(order[0] == RuntimeCallLane::trade_critical,
            "stale uncontended trade grants consumed the burst budget");
    require(order[1] == RuntimeCallLane::market_data,
            "market data did not follow the first contended trade grant");
}

} // namespace

int main() {
    try {
        trade_preempts_market_data();
        market_data_is_not_starved();
        uncontended_trade_grants_do_not_consume_burst();
        std::cout << "runtime lane tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

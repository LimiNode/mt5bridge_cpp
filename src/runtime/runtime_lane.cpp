/// \file runtime_lane.cpp
/// \brief Implements bounded-fair admission lanes for serialized runtime calls.

#include "runtime_lane.hpp"

#include <algorithm>

namespace mt5bridge::runtime {

RuntimeLane::RuntimeLane(std::size_t trade_burst_limit)
    : trade_burst_limit_(std::max<std::size_t>(1, trade_burst_limit)) {}

RuntimeLane::Permit::~Permit() {
    if (lane_)
        lane_->release(call_lane_);
}

RuntimeLane::Permit::Permit(Permit &&other) noexcept
    : lane_(other.lane_), call_lane_(other.call_lane_) {
    other.lane_ = nullptr;
}

RuntimeLane::Permit &RuntimeLane::Permit::operator=(Permit &&other) noexcept {
    if (this != &other) {
        if (lane_)
            lane_->release(call_lane_);
        lane_ = other.lane_;
        call_lane_ = other.call_lane_;
        other.lane_ = nullptr;
    }
    return *this;
}

RuntimeLane::Permit RuntimeLane::acquire(RuntimeCallLane lane) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t &waiters = lane == RuntimeCallLane::trade_critical
                               ? waiting_trade_critical_
                               : waiting_market_data_;
    ++waiters;
    try {
        condition_.wait(lock, [this, lane] {
            if (occupied_)
                return false;
            if (lane == RuntimeCallLane::trade_critical)
                return waiting_market_data_ == 0 ||
                       consecutive_trade_grants_ < trade_burst_limit_;
            return waiting_trade_critical_ == 0 ||
                   consecutive_trade_grants_ >= trade_burst_limit_;
        });
    } catch (...) {
        --waiters;
        throw;
    }
    --waiters;
    occupied_ = true;
    if (lane == RuntimeCallLane::trade_critical) {
        // Only grants made while market data was waiting consume the bounded
        // trade burst.  Uncontended trade calls must not leave stale priority
        // debt that penalizes the next market-data waiter.
        if (waiting_market_data_ != 0) {
            if (consecutive_trade_grants_ < trade_burst_limit_)
                ++consecutive_trade_grants_;
        } else {
            consecutive_trade_grants_ = 0;
        }
    } else {
        consecutive_trade_grants_ = 0;
    }
    return Permit(this, lane);
}

std::size_t RuntimeLane::waiting(RuntimeCallLane lane) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lane == RuntimeCallLane::trade_critical ? waiting_trade_critical_
                                                    : waiting_market_data_;
}

void RuntimeLane::release(RuntimeCallLane) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        occupied_ = false;
    }
    condition_.notify_all();
}

} // namespace mt5bridge::runtime

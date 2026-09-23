/// \file runtime_lane.hpp
/// \brief Defines bounded-fair admission lanes for serialized runtime calls.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace mt5bridge::runtime {

/// \enum RuntimeCallLane
/// \brief Classifies a serialized embedded-runtime call by operational urgency.
enum class RuntimeCallLane {
    market_data,
    trade_critical,
};

/// \class RuntimeLane
/// \brief Serializes runtime calls while giving trade-critical callers bounded priority.
///
/// A market-data call already in progress is never preempted. Once it releases
/// the lane, waiting trade-critical callers are admitted first. A configurable
/// trade burst limit prevents a continuously busy trade lane from starving
/// market-data progress.
class RuntimeLane {
public:
    /// \class Permit
    /// \brief Move-only ownership of one admitted runtime lane slot.
    class Permit {
    public:
        /// \brief Constructs an empty permit.
        Permit() = default;

        /// \brief Releases the lane slot owned by this permit.
        ~Permit();

        Permit(const Permit &) = delete;
        Permit &operator=(const Permit &) = delete;

        /// \brief Transfers an admitted slot from another permit.
        /// \param other Permit whose slot is transferred.
        Permit(Permit &&other) noexcept;

        /// \brief Replaces this permit with another admitted slot.
        /// \param other Permit whose slot is transferred.
        /// \return Reference to this permit.
        Permit &operator=(Permit &&other) noexcept;

        /// \brief Tests whether this object owns a lane slot.
        /// \return True when the permit is valid.
        explicit operator bool() const noexcept { return lane_ != nullptr; }

    private:
        friend class RuntimeLane;
        Permit(RuntimeLane *lane, RuntimeCallLane call_lane)
            : lane_(lane), call_lane_(call_lane) {}

        RuntimeLane *lane_ = nullptr;
        RuntimeCallLane call_lane_ = RuntimeCallLane::market_data;
    };

    /// \brief Constructs a scheduler with a bounded trade burst.
    /// \param trade_burst_limit Maximum consecutive trade grants while market
    /// data is waiting; zero is treated as one.
    explicit RuntimeLane(std::size_t trade_burst_limit = 4);

    /// \brief Waits until one call of the requested lane is admitted.
    /// \param lane Operational lane to acquire.
    /// \return Move-only permit releasing the slot on destruction.
    Permit acquire(RuntimeCallLane lane);

    /// \brief Reports how many callers are waiting in one lane.
    /// \param lane Lane whose waiters are counted.
    /// \return Current waiter count; the value is diagnostic only.
    std::size_t waiting(RuntimeCallLane lane) const;

private:
    friend class Permit;

    /// \brief Releases a permit and wakes the next eligible caller.
    /// \param lane Lane that was released.
    void release(RuntimeCallLane lane);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t waiting_market_data_ = 0;
    std::size_t waiting_trade_critical_ = 0;
    std::size_t consecutive_trade_grants_ = 0;
    std::size_t trade_burst_limit_ = 4;
    bool occupied_ = false;
};

} // namespace mt5bridge::runtime

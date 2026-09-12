/// \file owned_realtime_smoke.cpp
/// \brief Exercises the owned CPython realtime poller and finalization path.

#include <chrono>
#include <iostream>
#include <thread>

#include <mt5bridge.hpp>

struct SmokeState {
    bool ready = false;
    bool batch = false;
};

static int on_event(const Mt5SubscriptionEvent *event, void *context) {
    if (!event || !context)
        return 0;
    auto &state = *static_cast<SmokeState *>(context);
    if (event->type == MT5_SUBSCRIPTION_STATUS && event->status == MT5_SUBSCRIPTION_READY)
        state.ready = true;
    if (event->type == MT5_SUBSCRIPTION_TICK_BATCH && event->count != 0)
        state.batch = true;
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    try {
        mt5bridge::Client bridge(argv[1]);
        bridge.initialize();
        Mt5TickSourceRequest source{"EURUSD", 0, 0};
        Mt5SubscriptionRequest request{&source, 1, 10, 8, 8, 0, 0, {0, 0}};
        auto subscription = bridge.subscribe_ticks(request);
        SmokeState state;
        for (int i = 0; i < 100 && !(state.ready && state.batch); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            bridge.process_events(32, on_event, &state);
        }
        if (!state.ready || !state.batch)
            return 3;
        subscription.reset();
        bridge.shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

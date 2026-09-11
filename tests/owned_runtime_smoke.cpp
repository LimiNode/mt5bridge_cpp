/// \file owned_runtime_smoke.cpp
/// \brief Exercises the DLL-owned CPython lifecycle from a native consumer.

#include <iostream>
#include <string>
#include <thread>

#include <mt5bridge/client.hpp>

/// \brief Loads the bridge without a pre-existing Python interpreter.
/// \param argc Number of command-line arguments.
/// \param argv Command-line arguments; argv[1] is the DLL path.
/// \return Zero when initialize/eval/shutdown all succeed.
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::wcerr << L"usage: owned_runtime_smoke <mt5_bridge.dll>\n";
        return 2;
    }
    try {
        mt5bridge::Client bridge(argv[1]);
        mt5bridge::Client competing_client(argv[1]);
        bridge.initialize();
        try {
            competing_client.initialize();
            return 4;
        } catch (const std::runtime_error &) {
        }
        competing_client.shutdown();
        Mt5SubscriptionRequest subscription_request{"EURUSD", 0, 10, 1, 4, 0};
        auto subscription = bridge.subscribe_ticks(subscription_request);
        bool saw_subscription_status = false;
        auto on_event = [](const Mt5SubscriptionEvent *event, void *context) {
            if (event && context && event->type == MT5_SUBSCRIPTION_STATUS)
                *static_cast<bool *>(context) = true;
            return 0;
        };
        bridge.process_events(1, on_event, &saw_subscription_status);
        if (!saw_subscription_status)
            return 9;
        subscription.reset();
        const std::string response = bridge.eval(R"({"method":"terminal_info"})");
        if (response.find("owned_interpreter") == std::string::npos)
            return 3;
        const HMODULE module = LoadLibraryW(argv[1]);
        const auto raw_shutdown = module
            ? reinterpret_cast<int (*)()>(GetProcAddress(module, "mt5bridge_shutdown"))
            : nullptr;
        if (!raw_shutdown)
            return 5;
        int raw_shutdown_status = 0;
        std::thread raw_wrong_owner([&raw_shutdown_status, raw_shutdown] {
            raw_shutdown_status = raw_shutdown();
        });
        raw_wrong_owner.join();
        FreeLibrary(module);
        if (raw_shutdown_status == 0)
            return 6;
        bool rejected_cross_thread_shutdown = false;
        std::thread wrong_owner([&bridge, &rejected_cross_thread_shutdown] {
            try {
                bridge.shutdown();
            } catch (const std::runtime_error &) {
                rejected_cross_thread_shutdown = true;
            }
        });
        wrong_owner.join();
        if (!rejected_cross_thread_shutdown)
            return 7;
        if (bridge.eval(R"({"method":"terminal_info"})").find("owned_interpreter") ==
            std::string::npos)
            return 8;
        bridge.shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

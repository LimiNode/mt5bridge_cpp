/// \file smoke_no_mt5.cpp
/// \brief Exercises runtime loading and a minimal market-data control request.

#include <iostream>
#include <mt5bridge.hpp>

/// \brief Runs the basic bridge smoke scenario.
/// \return Zero on success; non-zero when initialization or evaluation fails.
int main() {
    mt5bridge::Client bridge;

    // Exercise loading and initialization; missing MT5 runtime is reported cleanly.
    try {
        bridge.load();
        bridge.initialize();
    } catch (const std::exception &error) {
        std::cerr << "Initialization failed: " << error.what() << std::endl;
        return 1;
    }

    // Issue a minimal request. Without a terminal the response will be
    // JSON null, which is still useful for exercising the pipeline.
    try {
        std::cout << bridge.eval(
                         R"({"method":"get_m1_bars","symbol":"EURUSD","count":1})")
                  << std::endl;
    } catch (const std::exception &error) {
        std::cerr << "mt5bridge_eval_json failed: " << error.what() << std::endl;
        bridge.shutdown();
        return 1;
    }
    bridge.shutdown();
    return 0;
}

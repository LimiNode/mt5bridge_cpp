/// \file usage_example.cpp
/// \brief Demonstrates loading the runtime and requesting terminal information.

#include <iostream>
#include <mt5bridge.hpp>

/// \brief Runs the terminal-information example.
/// \return Zero on success; non-zero when initialization or evaluation fails.
int main() {
    mt5bridge::Client bridge;

    // Initialize the bridge runtime (no custom Python home)
    try {
        bridge.load();
        bridge.initialize();
    } catch (const std::exception &error) {
        std::cerr << "Initialization failed: " << error.what() << std::endl;
        return 1;
    }

    try {
        std::cout << bridge.eval(R"({"method":"terminal_info"})") << std::endl;
    } catch (const std::exception &error) {
        std::cerr << "mt5bridge_eval_json failed: " << error.what() << std::endl;
        bridge.shutdown();
        return 1;
    }
    bridge.shutdown();
    return 0;
}

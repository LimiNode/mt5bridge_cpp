/// \file owned_runtime_smoke.cpp
/// \brief Exercises the DLL-owned CPython lifecycle from a native consumer.

#include <iostream>
#include <string>

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
        const std::string response = bridge.eval(R"({"method":"terminal_info"})");
        if (response.find("owned_interpreter") == std::string::npos)
            return 3;
        bridge.shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

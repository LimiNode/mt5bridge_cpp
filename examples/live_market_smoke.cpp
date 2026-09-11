/// \file live_market_smoke.cpp
/// \brief Runs a bounded native live check against an attached MetaTrader 5 terminal.

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <mt5bridge/client.hpp>

namespace {

/// \brief Prints a compact market-data diagnostic snapshot.
/// \param label Human-readable operation label.
/// \param diagnostics Snapshot returned by the bridge.
void print_diagnostics(const char *label, const Mt5FetchDiagnostics &diagnostics) {
    std::cout << label << " status=" << diagnostics.status
              << " complete=" << static_cast<unsigned>(diagnostics.complete)
              << " attempts=" << diagnostics.attempts
              << " retries=" << diagnostics.retries
              << " reconnects=" << diagnostics.reconnects
              << " warmup=" << static_cast<unsigned>(diagnostics.history_warmup_detected)
              << " last_error=" << diagnostics.last_mt5_error << '\n';
}

/// \brief Fails the smoke check when a range is not complete and non-empty.
/// \param label Human-readable operation label.
/// \param count Number of records returned by the operation.
/// \param diagnostics Snapshot returned by the bridge.
void require_complete(const char *label, std::size_t count,
                      const Mt5FetchDiagnostics &diagnostics) {
    if (count == 0 || diagnostics.status != MT5_FETCH_COMPLETE || !diagnostics.complete)
        throw std::runtime_error(std::string(label) +
                                 " did not return a complete non-empty range");
}

} // namespace

/// \brief Performs bounded terminal, tick-history, and bar-history checks.
/// \param argc Number of command-line arguments.
/// \param argv Optional symbol name (defaults to EURUSD).
/// \return Zero when all requested operations complete; non-zero on failure.
int main(int argc, char **argv) {
    const std::string symbol = argc > 1 ? argv[1] : "EURUSD";

    try {
        mt5bridge::Client bridge;
        bridge.load();
        bridge.initialize();

        std::cout << "terminal_info="
                  << bridge.eval(R"({"method":"terminal_info"})") << '\n';

        const auto now = std::chrono::system_clock::now();
        const auto now_msc = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch())
                                 .count();

        Mt5FetchDiagnostics tick_diagnostics{};
        const auto ticks = bridge.copy_ticks_range(symbol, now_msc - 10 * 60 * 1000, now_msc,
                                                   0, &tick_diagnostics);
        print_diagnostics("ticks", tick_diagnostics);
        std::cout << "ticks_count=" << ticks.size() << '\n';
        require_complete("ticks", ticks.size(), tick_diagnostics);

        Mt5FetchDiagnostics rate_diagnostics{};
        const auto rates = bridge.copy_rates_range(symbol, 1, now_msc - 60 * 60 * 1000, now_msc,
                                                   &rate_diagnostics);
        print_diagnostics("rates", rate_diagnostics);
        std::cout << "rates_count=" << rates.size() << '\n';
        require_complete("rates", rates.size(), rate_diagnostics);

        bridge.shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "live smoke failed: " << error.what() << '\n';
        return 1;
    }
}

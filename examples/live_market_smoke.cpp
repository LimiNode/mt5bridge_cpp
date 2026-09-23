/// \file live_market_smoke.cpp
/// \brief Runs a bounded native live check against an attached MetaTrader 5 terminal.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include <mt5bridge.hpp>

namespace {

/// \brief Bounded lookback used by the manual smoke for weekend-safe history.
constexpr auto kTickLookback = std::chrono::hours(72);
/// \brief Bounded M1 lookback that spans at least one completed session.
constexpr auto kRateLookback = std::chrono::hours(24 * 7);

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
        const auto ticks = bridge.copy_ticks_range(symbol, now_msc -
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           kTickLookback).count(), now_msc,
                                                   0, &tick_diagnostics);
        print_diagnostics("ticks", tick_diagnostics);
        std::cout << "ticks_count=" << ticks.size() << '\n';
        require_complete("ticks", ticks.size(), tick_diagnostics);

        const auto rate_period_msc = std::int64_t{60} * 1000;
        const auto rate_to_msc = (now_msc / rate_period_msc) * rate_period_msc;
        const auto rate_from_msc =
            ((rate_to_msc - std::chrono::duration_cast<std::chrono::milliseconds>(
                                  kRateLookback).count()) /
             rate_period_msc) *
            rate_period_msc;
        const auto rates = bridge.query_rates_range(symbol, 1, rate_from_msc, rate_to_msc);
        print_diagnostics("rates", rates.diagnostics);
        std::cout << "rates_count=" << rates.values.size() << '\n';
        require_complete("rates", rates.values.size(), rates.diagnostics);

        bridge.shutdown();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "live smoke failed: " << error.what() << '\n';
        return 1;
    }
}

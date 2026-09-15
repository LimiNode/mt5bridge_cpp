/// \file trade_observation_example.cpp
/// \brief Demonstrates the typed Stage 1 account, symbol, and order-check API.

#include <mt5bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

/// \brief Numeric MT5 action value for a market deal request.
constexpr std::uint32_t kTradeActionDeal = 1;
/// \brief Numeric MT5 order type value for a buy request.
constexpr std::uint32_t kOrderTypeBuy = 0;
/// \brief Symbol capability bit for fill-or-kill requests.
constexpr std::uint32_t kSymbolFillingFok = 1;
/// \brief Symbol capability bit for immediate-or-cancel requests.
constexpr std::uint32_t kSymbolFillingIoc = 2;
/// \brief Symbol execution value for market execution.
constexpr std::uint32_t kSymbolExecutionMarket = 2;

/// \brief Chooses a basic market-order filling mode from known symbol capabilities.
/// \param capabilities Typed symbol capability snapshot.
/// \return MT5 ORDER_FILLING_* value suitable for an advisory market check.
std::uint32_t choose_market_filling(const Mt5SymbolCapabilities &capabilities) {
    if ((capabilities.known_fields & MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE) != 0) {
        if ((capabilities.filling_mode & kSymbolFillingFok) != 0)
            return 0;
        if ((capabilities.filling_mode & kSymbolFillingIoc) != 0)
            return 1;
    }
    if ((capabilities.known_fields & MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE) != 0 &&
        capabilities.trade_exemode != kSymbolExecutionMarket)
        return 2;
    return 0;
}

/// \brief Prints a bounded typed observation sequence against one terminal.
/// \param symbol UTF-8 symbol name used for capability and check requests.
/// \param dll_path Runtime DLL path supplied by the caller.
/// \return Process exit status.
int run(const std::string &symbol, const wchar_t *dll_path) {
    mt5bridge::Client bridge(dll_path);
    bridge.initialize();

    const auto account = bridge.account_info();
    std::cout << "account: " << account.server << " login=" << account.login
              << " currency=" << account.currency << " balance=" << account.balance
              << " equity=" << account.equity << '\n';
    std::cout << "account known mask: 0x" << std::hex << account.known_fields
              << std::dec << '\n';

    const auto capabilities = bridge.symbol_capabilities(symbol);
    std::cout << "symbol: " << capabilities.symbol
              << " trade_mode=" << capabilities.trade_mode
              << " trade_exemode=" << capabilities.trade_exemode
              << " order_mode=0x" << std::hex << capabilities.order_mode
              << std::dec << " filling_mode=0x" << std::hex
              << capabilities.filling_mode << std::dec << '\n';
    std::cout << "volume range: " << capabilities.volume_min << ".."
              << capabilities.volume_max << " step=" << capabilities.volume_step
              << " closeby=" << capabilities.closeby_allowed << '\n';
    std::cout << "symbol known mask: 0x" << std::hex << capabilities.known_fields
              << std::dec << '\n';

    // order_check is advisory and side-effect free.  It validates the request
    // but does not submit an order; Stage 2 will add durable order dispatch.
    double check_price = 0.0;
    double check_volume = 0.0;
    const auto now_msc = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    try {
        const auto ticks = bridge.copy_ticks_range(symbol, now_msc - 10 * 60 * 1000, now_msc);
        if (!ticks.empty() &&
            (capabilities.known_fields & MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS) != 0) {
            check_price = ticks.back().ask;
            check_volume = capabilities.volume_min;
        } else {
            std::cerr << "recent price or volume limits unavailable; using an "
                         "invalid-volume check\n";
        }
    } catch (const std::exception &error) {
        std::cerr << "recent tick unavailable; using an invalid-volume check: "
                  << error.what() << '\n';
    }

    Mt5OrderCheckRequest request{};
    request.symbol_utf8 = symbol.c_str();
    // MT5 order comments are commonly limited to 31 UTF-8 bytes.
    request.comment_utf8 = "mt5bridge observation";
    request.volume = check_volume;
    request.price = check_price;
    request.action = kTradeActionDeal;
    request.type = kOrderTypeBuy;
    request.type_filling = choose_market_filling(capabilities);
    request.type_time = 0;
    const auto check = bridge.order_check(request);
    std::cout << "order_check: retcode=" << check.retcode
              << " comment=" << check.comment << '\n';

    // Snapshot calls are observation-only and preserve the identifiers needed
    // by the future reconciliation graph. Empty collections are valid.
    try {
        const auto active_orders = bridge.orders(Mt5OrdersRequest{symbol.c_str(), nullptr, 0, 0});
        const auto active_positions =
            bridge.positions(Mt5PositionsRequest{symbol.c_str(), nullptr, 0, 0, 0});
        const auto history_from = now_msc - 60 * 60 * 1000;
        const Mt5HistoryOrdersRequest history_orders_request{
            history_from, now_msc, nullptr, 0, 0, 0};
        const Mt5HistoryDealsRequest history_deals_request{
            history_from, now_msc, nullptr, 0, 0, 0, 0};
        const auto history_orders = bridge.history_orders(history_orders_request);
        const auto history_deals = bridge.history_deals(history_deals_request);
        std::cout << "snapshots: orders=" << active_orders.size()
                  << " positions=" << active_positions.size()
                  << " history_orders=" << history_orders.size()
                  << " history_deals=" << history_deals.size() << '\n';
    } catch (const std::exception &error) {
        std::cerr << "trade snapshots unavailable: " << error.what() << '\n';
    }

    bridge.shutdown();
    return 0;
}

} // namespace

/// \brief Runs the typed trade-observation example.
/// \param argc Number of command-line arguments.
/// \param argv Optional symbol name (defaults to EURUSD).
/// \return Zero after a successful advisory observation; non-zero on failure.
int main(int argc, char **argv) {
    const std::string symbol = argc > 1 ? argv[1] : "EURUSD";
    try {
        return run(symbol, L"mt5_bridge.dll");
    } catch (const std::exception &error) {
        std::cerr << "trade observation failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

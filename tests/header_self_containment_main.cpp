/// \file header_self_containment_main.cpp
/// \brief Links the focused public-header self-containment probes.

extern "C" int mt5bridge_probe_market_common_header();
extern "C" int mt5bridge_probe_market_ticks_header();
extern "C" int mt5bridge_probe_market_rates_header();
extern "C" int mt5bridge_probe_market_realtime_header();
extern "C" int mt5bridge_probe_trade_common_header();
extern "C" int mt5bridge_probe_trade_account_header();
extern "C" int mt5bridge_probe_trade_symbol_header();
extern "C" int mt5bridge_probe_trade_order_check_header();
extern "C" int mt5bridge_probe_trade_orders_header();
extern "C" int mt5bridge_probe_trade_positions_header();
extern "C" int mt5bridge_probe_trade_deals_header();
extern "C" int mt5bridge_probe_reconciliation_graph_header();
extern "C" int mt5bridge_probe_reconciliation_engine_header();
extern "C" int mt5bridge_probe_reconciliation_coordinator_header();
extern "C" int mt5bridge_probe_reconciliation_worker_header();
extern "C" int mt5bridge_probe_dispatch_operation_worker_header();
extern "C" int mt5bridge_probe_environment_consistency_header();
extern "C" int mt5bridge_probe_dispatch_journal_header();
extern "C" int mt5bridge_probe_file_journal_store_header();

int main() {
    return mt5bridge_probe_market_common_header() |
           mt5bridge_probe_market_ticks_header() |
           mt5bridge_probe_market_rates_header() |
           mt5bridge_probe_market_realtime_header() |
           mt5bridge_probe_trade_common_header() |
           mt5bridge_probe_trade_account_header() |
           mt5bridge_probe_trade_symbol_header() |
           mt5bridge_probe_trade_order_check_header() |
           mt5bridge_probe_trade_orders_header() |
           mt5bridge_probe_trade_positions_header() |
           mt5bridge_probe_trade_deals_header() |
           mt5bridge_probe_reconciliation_graph_header() |
           mt5bridge_probe_reconciliation_engine_header() |
           mt5bridge_probe_reconciliation_coordinator_header() |
           mt5bridge_probe_reconciliation_worker_header() |
           mt5bridge_probe_dispatch_operation_worker_header() |
           mt5bridge_probe_environment_consistency_header() |
           mt5bridge_probe_dispatch_journal_header() |
           mt5bridge_probe_file_journal_store_header();
}

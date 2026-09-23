/// \file header_self_containment_main.cpp
/// \brief Links the focused public-header self-containment probes.

extern "C" int mt5bridge_probe_market_data_header();
extern "C" int mt5bridge_probe_trade_observation_header();
extern "C" int mt5bridge_probe_observation_graph_header();
extern "C" int mt5bridge_probe_reconciliation_engine_header();
extern "C" int mt5bridge_probe_observation_coordinator_header();
extern "C" int mt5bridge_probe_observation_worker_header();
extern "C" int mt5bridge_probe_environment_consistency_header();
extern "C" int mt5bridge_probe_dispatch_journal_header();
extern "C" int mt5bridge_probe_file_journal_store_header();

int main() {
    return mt5bridge_probe_market_data_header() |
           mt5bridge_probe_trade_observation_header() |
           mt5bridge_probe_observation_graph_header() |
           mt5bridge_probe_reconciliation_engine_header() |
           mt5bridge_probe_observation_coordinator_header() |
           mt5bridge_probe_observation_worker_header() |
           mt5bridge_probe_environment_consistency_header() |
           mt5bridge_probe_dispatch_journal_header() |
           mt5bridge_probe_file_journal_store_header();
}

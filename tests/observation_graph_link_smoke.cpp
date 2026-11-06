/// \file observation_graph_link_smoke.cpp
/// \brief Verifies that the public observation graph links through CMake.

#include <mt5bridge/reconciliation/graph.hpp>

int main() {
    const mt5bridge::AccountKey account{"link-smoke", 1};
    mt5bridge::ObservationGraph graph(account);
    return graph.account_key() == account && graph.instance_id() != 0 ? 0 : 1;
}

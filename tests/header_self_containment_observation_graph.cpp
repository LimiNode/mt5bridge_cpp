/// \file header_self_containment_observation_graph.cpp
/// \brief Compiles the observation graph header as a standalone translation unit.

#include <mt5bridge/observation/graph.hpp>

extern "C" int mt5bridge_probe_observation_graph_header() {
    return 0;
}

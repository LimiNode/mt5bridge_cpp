/// \file header_self_containment_observation_coordinator.cpp
/// \brief Compiles the observation coordinator header as a standalone translation unit.

#include <mt5bridge/observation/coordinator.hpp>

extern "C" int mt5bridge_probe_observation_coordinator_header() {
    return 0;
}

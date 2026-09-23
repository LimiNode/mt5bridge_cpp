/// \file header_self_containment_observation_worker.cpp
/// \brief Compiles the reconciliation worker header as a standalone translation unit.

#include <mt5bridge/observation/worker.hpp>

extern "C" int mt5bridge_probe_observation_worker_header() {
    return 0;
}

/// \file header_self_containment_reconciliation_coordinator.cpp
/// \brief Compiles the reconciliation coordinator header as a standalone translation unit.

#include <mt5bridge/reconciliation/coordinator.hpp>

extern "C" int mt5bridge_probe_reconciliation_coordinator_header() {
    return 0;
}

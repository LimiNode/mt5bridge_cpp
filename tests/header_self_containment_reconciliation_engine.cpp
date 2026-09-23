/// \file header_self_containment_reconciliation_engine.cpp
/// \brief Compiles the reconciliation engine header as a standalone translation unit.

#include <mt5bridge/reconciliation/engine.hpp>

extern "C" int mt5bridge_probe_reconciliation_engine_header() {
    return 0;
}

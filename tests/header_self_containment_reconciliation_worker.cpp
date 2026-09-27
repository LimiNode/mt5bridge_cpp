/// \file header_self_containment_reconciliation_worker.cpp
/// \brief Compiles the reconciliation worker header as a standalone translation unit.

#include <mt5bridge/reconciliation/worker.hpp>

extern "C" int mt5bridge_probe_reconciliation_worker_header() {
    return 0;
}

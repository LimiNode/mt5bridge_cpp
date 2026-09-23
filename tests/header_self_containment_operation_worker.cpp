/// \file header_self_containment_operation_worker.cpp
/// \brief Compiles the journal-aware operation worker header standalone.

#include <mt5bridge/reconciliation/operation_worker.hpp>

extern "C" int mt5bridge_probe_operation_worker_header() { return 0; }

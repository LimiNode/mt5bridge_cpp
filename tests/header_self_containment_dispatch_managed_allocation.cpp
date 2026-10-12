/// \file header_self_containment_dispatch_managed_allocation.cpp
/// \brief Compiles the managed allocation public header in isolation.

#include <mt5bridge/dispatch/managed_allocation.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_allocation_header() { return 0; }

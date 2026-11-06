/// \file header_self_containment_dispatch_managed_opening_allocation.cpp
/// \brief Checks that the private B3C allocation header is self-contained.

#include "managed_opening_allocation.hpp"

extern "C" int mt5bridge_probe_managed_opening_allocation_header() { return 0; }

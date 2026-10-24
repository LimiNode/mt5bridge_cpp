/// \file header_self_containment_dispatch_managed_settlement_preparation.cpp
/// \brief Checks standalone inclusion of the staged settlement contract.

#include <mt5bridge/dispatch/managed_settlement_preparation.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_settlement_preparation_header() {
    return 0;
}

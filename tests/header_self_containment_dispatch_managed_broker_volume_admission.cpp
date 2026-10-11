/// \file header_self_containment_dispatch_managed_broker_volume_admission.cpp
/// \brief Compiles the managed broker-volume admission header independently.

#include <mt5bridge/dispatch/managed_broker_volume_admission.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_broker_volume_admission_header() {
    return 0;
}

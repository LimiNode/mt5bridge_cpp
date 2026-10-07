/// \file header_self_containment_dispatch_broker_reversal_reconciliation.cpp
/// \brief Compiles the graph-to-broker-reversal adapter standalone.

#include <mt5bridge/dispatch/broker_reversal_reconciliation.hpp>

extern "C" int mt5bridge_probe_dispatch_broker_reversal_reconciliation_header() {
    return mt5bridge::BrokerVolumeNormalization{}.valid() ? 0 : 0;
}

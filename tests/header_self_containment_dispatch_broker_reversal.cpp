/// \file header_self_containment_dispatch_broker_reversal.cpp
/// \brief Compiles the broker reversal contract standalone.

#include <mt5bridge/dispatch/broker_reversal.hpp>
#include <mt5bridge/dispatch/file_broker_reversal_store.hpp>

extern "C" int mt5bridge_probe_dispatch_broker_reversal_header() {
    const mt5bridge::BrokerReversalScanResult result{};
    return result.complete() ? 1 : 0;
}

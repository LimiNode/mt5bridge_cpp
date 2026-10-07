/// \file header_self_containment_dispatch_broker_allocation_envelope.cpp
/// \brief Compiles the broker-allocation envelope headers in isolation.

#include <mt5bridge/dispatch/broker_allocation_envelope.hpp>
#include <mt5bridge/dispatch/file_broker_allocation_store.hpp>

extern "C" int mt5bridge_probe_dispatch_broker_allocation_envelope_header() {
    const mt5bridge::BrokerAllocationScanResult result{};
    return result.records.empty() ? 0 : 1;
}

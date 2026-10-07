/// \file header_self_containment_dispatch_managed_allocation.cpp
/// \brief Compiles the managed-allocation public headers in isolation.

#include <mt5bridge/dispatch/managed_allocation.hpp>
#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_allocation_header() {
    const mt5bridge::ManagedAllocationScanResult result{};
    return result.records.empty() ? 0 : 1;
}

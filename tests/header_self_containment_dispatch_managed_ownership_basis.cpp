/// \file header_self_containment_dispatch_managed_ownership_basis.cpp
/// \brief Compiles managed-ownership-basis headers in isolation.

#include <mt5bridge/dispatch/file_managed_ownership_basis_store.hpp>
#include <mt5bridge/dispatch/managed_ownership_basis.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_ownership_basis_header() {
    const mt5bridge::ManagedOwnershipBasisScanResult result{};
    return result.complete() ? 0 : 0;
}

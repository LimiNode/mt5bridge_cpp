/// \file header_self_containment_dispatch_managed_deal_settlement.cpp
/// \brief Verifies standalone inclusion of the managed-deal settlement header.

#include <mt5bridge/dispatch/managed_deal_settlement.hpp>

extern "C" int mt5bridge_probe_dispatch_managed_deal_settlement_header() {
    const mt5bridge::ManagedDealSettlementLoadResult result{};
    return result.found() ? 1 : 0;
}

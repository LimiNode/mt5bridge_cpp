/// \file header_self_containment_file_managed_deal_settlement_store.cpp
/// \brief Verifies standalone inclusion of the managed-deal store header.

#include <mt5bridge/dispatch/file_managed_deal_settlement_store.hpp>

extern "C" int mt5bridge_probe_file_managed_deal_settlement_store_header() {
    const mt5bridge::ManagedDealSettlementScanResult result{};
    return result.complete() ? 1 : 0;
}

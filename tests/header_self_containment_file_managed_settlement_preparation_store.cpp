/// \file header_self_containment_file_managed_settlement_preparation_store.cpp
/// \brief Checks standalone inclusion of the staged settlement file store.

#include <mt5bridge/dispatch/file_managed_settlement_preparation_store.hpp>

extern "C" int mt5bridge_probe_file_managed_settlement_preparation_store_header() {
    return 0;
}

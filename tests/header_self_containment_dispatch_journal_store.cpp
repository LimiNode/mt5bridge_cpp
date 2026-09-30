/// \file header_self_containment_dispatch_journal_store.cpp
/// \brief Compiles the durable journal store contract standalone.

#include <mt5bridge/dispatch/journal_store.hpp>

extern "C" int mt5bridge_probe_dispatch_journal_store_header() {
    const mt5bridge::StoreLoadResult result{};
    return result.found() ? 1 : 0;
}

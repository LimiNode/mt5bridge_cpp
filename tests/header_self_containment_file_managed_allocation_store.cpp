/// \file header_self_containment_file_managed_allocation_store.cpp
/// \brief Compiles the file-backed managed allocation header in isolation.

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>

extern "C" int mt5bridge_probe_file_managed_allocation_store_header() { return 0; }

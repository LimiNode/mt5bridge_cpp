/// \file header_self_containment_file_journal_store.cpp
/// \brief Compiles the file-journal-store header as a standalone translation unit.

#include <mt5bridge/dispatch/file_journal_store.hpp>

extern "C" int mt5bridge_probe_file_journal_store_header() {
    return 0;
}

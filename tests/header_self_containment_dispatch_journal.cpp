/// \file header_self_containment_dispatch_journal.cpp
/// \brief Compiles the dispatch-journal header as a standalone translation unit.

#include <mt5bridge/dispatch/journal.hpp>

extern "C" int mt5bridge_probe_dispatch_journal_header() {
    return 0;
}

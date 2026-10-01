/// \file header_self_containment_dispatch_admission.cpp
/// \brief Compiles the durable admission contract as a standalone translation unit.

#include <mt5bridge/dispatch/admission.hpp>

extern "C" int mt5bridge_probe_dispatch_admission_header() {
    return 0;
}

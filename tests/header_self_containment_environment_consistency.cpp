/// \file header_self_containment_environment_consistency.cpp
/// \brief Compiles the environment-consistency header as a standalone translation unit.

#include <mt5bridge/observation/environment_consistency.hpp>

extern "C" int mt5bridge_probe_environment_consistency_header() {
    return 0;
}

/// \file header_self_containment_dispatch_managed_opening_provenance_producer.cpp
/// \brief Checks that the private B3D producer header is self-contained.

#include "managed_opening_provenance_producer.hpp"

extern "C" int mt5bridge_probe_managed_opening_provenance_producer_header() {
    return 0;
}

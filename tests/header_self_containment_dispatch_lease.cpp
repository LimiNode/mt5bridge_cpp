/// \file header_self_containment_dispatch_lease.cpp
/// \brief Compiles the single-writer lease contract standalone.

#include <mt5bridge/dispatch/lease.hpp>

namespace {

class ProbeLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &) const override {
        return std::nullopt;
    }
};

} // namespace

extern "C" int mt5bridge_probe_dispatch_lease_header() {
    ProbeLease lease;
    return lease.held_fencing_token(mt5bridge::AccountKey{}).has_value() ? 1 : 0;
}

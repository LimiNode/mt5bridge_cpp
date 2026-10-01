/// \file dispatch_admission_header_only_test.cpp
/// \brief Verifies the admission barrier remains usable without the journal target.

#include <mt5bridge/dispatch/admission.hpp>

#include <cstdint>
#include <optional>

namespace {

class CustomStore final : public mt5bridge::DurableJournalStore {
public:
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &,
        std::optional<std::uint64_t>) override {
        return mt5bridge::StoreCommitStatus::io_error;
    }

    mt5bridge::StoreLoadResult load(const mt5bridge::OperationKey &) const override {
        return {};
    }

    mt5bridge::StoreScanResult scan() const override { return {}; }
};

class CustomLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &) const override {
        return std::nullopt;
    }
};

} // namespace

int main() {
    CustomStore store;
    mt5bridge::OperationJournal journal(store);
    mt5bridge::ObservationGraph graph;
    mt5bridge::DispatchAdmissionBarrier barrier(
        journal, graph, mt5bridge::EnvironmentConsistencyRequest{});
    CustomLease lease;
    const auto result = barrier.admit({}, {}, lease);
    return result.status == mt5bridge::DispatchAdmissionStatus::invalid_request ? 0 : 1;
}

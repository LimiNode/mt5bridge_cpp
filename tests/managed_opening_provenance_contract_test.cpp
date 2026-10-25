/// \file managed_opening_provenance_contract_test.cpp
/// \brief Verifies the C3A opening-provenance trust boundary.

#include "managed_opening_provenance_contract.hpp"

#include <cassert>
#include <type_traits>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

AccountKey account() { return {"demo", 42}; }

ManagedOpeningProvenanceRequest request() {
    return {OperationKey{account(), 71, 9}, OperationKind::open, 4, 88001};
}

class ForgedFoundSource final : public DurableManagedOpeningProvenanceSource {
public:
    ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceRequest &) const override {
        // A status alone is not authority. The result has no sealed proof.
        return {ManagedOpeningProvenanceStatus::found, std::nullopt};
    }
};

void rejects_structural_lookup_without_source() {
    const auto query = request();
    assert(query.valid());

    UnavailableManagedOpeningProvenanceSource source;
    const auto result = source.verify(query);
    assert(result.status == ManagedOpeningProvenanceStatus::unavailable);
    assert(!result.found());
    assert(!result.proof);
}

void rejects_invalid_query_before_source_lookup() {
    auto query = request();
    query.operation_revision = 0;
    UnavailableManagedOpeningProvenanceSource source;
    const auto result = source.verify(query);
    assert(result.status == ManagedOpeningProvenanceStatus::invalid_request);
    assert(!result.found());

    query = request();
    query.operation_kind = OperationKind::close;
    const auto wrong_kind = source.verify(query);
    assert(wrong_kind.status == ManagedOpeningProvenanceStatus::invalid_request);
}

void rejects_found_status_without_sealed_proof() {
    ForgedFoundSource source;
    const auto result = source.verify(request());
    assert(result.status == ManagedOpeningProvenanceStatus::found);
    assert(!result.found());
}

void proof_is_not_caller_constructible() {
    static_assert(!std::is_default_constructible_v<VerifiedManagedOpeningAllocation>);
    static_assert(!std::is_constructible_v<VerifiedManagedOpeningAllocation,
                                          OperationKey, std::uint64_t,
                                          std::uint64_t, std::uint64_t,
                                          std::uint64_t>);
}

} // namespace

int main() {
    rejects_structural_lookup_without_source();
    rejects_invalid_query_before_source_lookup();
    rejects_found_status_without_sealed_proof();
    proof_is_not_caller_constructible();
    return 0;
}

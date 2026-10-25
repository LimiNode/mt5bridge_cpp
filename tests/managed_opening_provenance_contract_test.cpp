/// \file managed_opening_provenance_contract_test.cpp
/// \brief Verifies the C3A opening-provenance trust boundary.

#include "managed_opening_provenance_contract.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

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
    require(query.valid(), "valid opening lookup was rejected");

    UnavailableManagedOpeningProvenanceSource source;
    const auto result = source.verify(query);
    require(result.status == ManagedOpeningProvenanceStatus::unavailable,
            "missing opening producer did not fail closed");
    require(!result.found(), "unavailable opening source produced a proof");
    require(!result.proof, "unavailable opening source returned proof data");
}

void rejects_invalid_query_before_source_lookup() {
    auto query = request();
    query.operation_revision = 0;
    UnavailableManagedOpeningProvenanceSource source;
    const auto result = source.verify(query);
    require(result.status == ManagedOpeningProvenanceStatus::invalid_request,
            "zero opening revision was accepted");
    require(!result.found(), "invalid opening request produced a proof");

    query = request();
    query.operation_kind = OperationKind::close;
    const auto wrong_kind = source.verify(query);
    require(wrong_kind.status == ManagedOpeningProvenanceStatus::invalid_request,
            "non-open operation kind was accepted");
}

void rejects_found_status_without_sealed_proof() {
    ForgedFoundSource source;
    const auto result = source.verify(request());
    require(result.status == ManagedOpeningProvenanceStatus::found,
            "forged source fixture did not return its test status");
    require(!result.found(), "found status without sealed proof was accepted");
}

void caller_query_fields_do_not_create_proof() {
    auto query = request();
    query.operation_key = OperationKey{account(), 9001, 9002};
    query.operation_revision = 77;
    query.position_identifier = 99001;
    UnavailableManagedOpeningProvenanceSource source;
    const auto result = source.verify(query);
    require(result.status == ManagedOpeningProvenanceStatus::unavailable,
            "caller identity fields created opening authority");
    require(!result.found() && !result.proof,
            "caller identity fields returned a sealed proof");
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
    try {
        rejects_structural_lookup_without_source();
        rejects_invalid_query_before_source_lookup();
        rejects_found_status_without_sealed_proof();
        caller_query_fields_do_not_create_proof();
        proof_is_not_caller_constructible();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

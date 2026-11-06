#pragma once

/// \file dispatch/managed_opening_provenance_contract.hpp
/// \brief Defines the private C3A trust-boundary contract for opening evidence.

#include <mt5bridge/dispatch/journal_types.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace mt5bridge::dispatch {

/// \enum ManagedOpeningProofScope
/// \brief States whether a sealed opening proof is historical or current.
enum class ManagedOpeningProofScope : std::uint8_t {
    historical = 0, ///< Exact immutable frontier; later fills may exist.
    current = 1,    ///< Exact immutable frontier matching fresh B3B evidence.
};

/// \brief Tests whether a proof scope is defined.
/// \param scope Candidate scope.
/// \return True only for a known proof scope.
constexpr bool valid_managed_opening_proof_scope(ManagedOpeningProofScope scope) {
    return scope == ManagedOpeningProofScope::historical ||
           scope == ManagedOpeningProofScope::current;
}

/// \struct ManagedOpeningProvenanceRequest
/// \brief Identifies a durable opening-evidence lookup.
///
/// Every field is a query constraint. None of the values is accepted as proof
/// merely because a caller supplied it.
struct ManagedOpeningProvenanceRequest {
    OperationKey operation_key; ///< Candidate managed OPEN operation identity.
    OperationKind operation_kind = OperationKind::open; ///< Expected journal kind.
    std::uint64_t operation_revision = 0; ///< Expected committed journal revision.
    std::uint64_t position_identifier = 0; ///< Expected broker POSITION_IDENTIFIER.

    /// \brief Tests whether the lookup identity is usable.
    /// \return True only for a complete account-scoped query.
    bool valid() const {
        return operation_key.valid() && operation_kind == OperationKind::open &&
               operation_revision != 0 && position_identifier != 0;
    }
};

/// \enum ManagedOpeningProvenanceLookupKind
/// \brief Selects historical identity lookup or current frontier lookup.
enum class ManagedOpeningProvenanceLookupKind : std::uint8_t {
    exact_historical = 0, ///< Requires the complete immutable frontier key.
    current = 1,          ///< Requires the unique source-complete current frontier.
};

/// \struct ManagedOpeningProvenanceSelector
/// \brief Explicit private selector for trusted opening proof lookup.
///
/// `operation_revision` is the immutable journal `result_source_revision`.
/// The B3D producer derives the current lifecycle revision separately before
/// invoking the existing B3B verifier API.
struct ManagedOpeningProvenanceSelector {
    ManagedOpeningProvenanceLookupKind kind =
        ManagedOpeningProvenanceLookupKind::exact_historical;
    OperationKey operation_key;
    std::uint64_t operation_revision = 0; ///< Immutable result-source revision.
    std::uint64_t frontier_revision = 0; ///< Required only for exact lookup.
    std::uint64_t position_identifier = 0;

    /// \brief Tests whether the explicit selector is complete.
    /// \return True only when all fields required by its kind are present.
    bool valid() const {
        if (!operation_key.valid() || operation_revision == 0 ||
            position_identifier == 0)
            return false;
        if (kind == ManagedOpeningProvenanceLookupKind::exact_historical)
            return frontier_revision != 0;
        return kind == ManagedOpeningProvenanceLookupKind::current &&
               frontier_revision == 0;
    }
};

/// \class VerifiedManagedOpeningAllocation
/// \brief Sealed proof of a committed managed opening allocation.
///
/// The value cannot be constructed from a caller-supplied identity, revision,
/// volume, or digest. A future trusted producer must be added explicitly after
/// it validates the complete durable evidence chain described by ADR-0038.
class VerifiedManagedOpeningAllocation {
public:
    /// \brief Returns the committed OPEN operation identity.
    /// \return Account-scoped operation key.
    const OperationKey &operation_key() const { return operation_key_; }

    /// \brief Returns the journal revision backed by the proof.
    /// \return Non-zero committed revision.
    std::uint64_t operation_revision() const { return operation_revision_; }

    /// \brief Returns the broker position identity backed by the proof.
    /// \return Non-zero POSITION_IDENTIFIER.
    std::uint64_t position_identifier() const { return position_identifier_; }

    /// \brief Returns the immutable frontier discriminator checked by the proof.
    /// \return Non-zero durable frontier identity.
    std::uint64_t frontier_revision() const { return frontier_revision_; }

    /// \brief Returns the semantic scope of the proof.
    /// \return Historical or current lookup scope.
    ManagedOpeningProofScope scope() const { return scope_; }

    /// \brief Returns the committed managed logical units.
    /// \return Positive logical quantity.
    std::uint64_t managed_logical_units() const { return managed_logical_units_; }

    /// \brief Returns the canonical evidence-chain digest.
    /// \return Non-zero deterministic source identity.
    std::uint64_t evidence_digest() const { return evidence_digest_; }

    /// \brief Tests the sealed proof's local shape.
    /// \return True for a complete proof value.
    bool valid() const {
        return operation_key_.valid() && operation_revision_ != 0 &&
               position_identifier_ != 0 && frontier_revision_ != 0 &&
               managed_logical_units_ != 0 && evidence_digest_ != 0 &&
               valid_managed_opening_proof_scope(scope_);
    }

private:
    VerifiedManagedOpeningAllocation(OperationKey operation_key,
                                     std::uint64_t operation_revision,
                                     std::uint64_t position_identifier,
                                     std::uint64_t frontier_revision,
                                     std::uint64_t managed_logical_units,
                                     std::uint64_t evidence_digest,
                                     ManagedOpeningProofScope scope)
        : operation_key_(std::move(operation_key)),
          operation_revision_(operation_revision),
          position_identifier_(position_identifier),
          frontier_revision_(frontier_revision),
          managed_logical_units_(managed_logical_units),
          evidence_digest_(evidence_digest), scope_(scope) {}

    OperationKey operation_key_;
    std::uint64_t operation_revision_ = 0;
    std::uint64_t position_identifier_ = 0;
    std::uint64_t frontier_revision_ = 0;
    std::uint64_t managed_logical_units_ = 0;
    std::uint64_t evidence_digest_ = 0;
    ManagedOpeningProofScope scope_ = ManagedOpeningProofScope::historical;

    // Construction remains private to the trusted producer. Caller-provided
    // identities and quantities cannot manufacture this value.
    friend class ManagedOpeningProvenanceProducer;
};

/// \enum ManagedOpeningProvenanceStatus
/// \brief Reports one durable opening-provenance lookup outcome.
enum class ManagedOpeningProvenanceStatus : std::uint8_t {
    found = 0, ///< A trusted producer returned a sealed proof.
    invalid_request = 1, ///< The lookup identity is malformed.
    unavailable = 2, ///< No matching durable frontier is currently available.
    invalid_evidence = 3, ///< A producer found malformed or mismatched evidence.
    io_error = 4, ///< Durable source I/O failed.
    lease_not_held = 5, ///< Account fencing was lost before the result boundary.
};

/// \struct ManagedOpeningProvenanceResult
/// \brief Carries a status-bearing sealed opening proof lookup.
struct ManagedOpeningProvenanceResult {
    ManagedOpeningProvenanceStatus status =
        ManagedOpeningProvenanceStatus::unavailable;
    std::optional<VerifiedManagedOpeningAllocation> proof;

    /// \brief Tests whether a usable sealed proof was returned.
    /// \return True only for a complete proof and the `found` status.
    bool found() const {
        return status == ManagedOpeningProvenanceStatus::found && proof &&
               proof->valid();
    }
};

/// \class DurableManagedOpeningProvenanceSource
/// \brief Private lookup seam for independently verified opening evidence.
class DurableManagedOpeningProvenanceSource {
public:
    virtual ~DurableManagedOpeningProvenanceSource() = default;

    /// \brief Looks up a proof from durable committed sources.
    /// \param request Query constraints, never proof fields.
    /// \return A sealed proof or a fail-closed status.
    virtual ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceRequest &request) const = 0;

    /// \brief Looks up a proof using an explicit historical/current selector.
    /// \param selector Private lookup scope and immutable identity constraints.
    /// \return A sealed proof or a fail-closed status.
    virtual ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceSelector &selector) const {
        if (!selector.valid())
            return {ManagedOpeningProvenanceStatus::invalid_request, std::nullopt};
        return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
    }
};

/// \class UnavailableManagedOpeningProvenanceSource
/// \brief Explicit fail-closed source used until C3A durable evidence exists.
class UnavailableManagedOpeningProvenanceSource final
    : public DurableManagedOpeningProvenanceSource {
public:
    using DurableManagedOpeningProvenanceSource::verify;

    /// \brief Rejects every valid lookup because no producer exists yet.
    /// \param request Query constraints to validate before failing closed.
    /// \return `invalid_request` for malformed input, otherwise `unavailable`.
    ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceRequest &request) const override {
        if (!request.valid())
            return {ManagedOpeningProvenanceStatus::invalid_request, std::nullopt};
        return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
    }
};

} // namespace mt5bridge::dispatch

#pragma once

/// \file dispatch/managed_opening_source_verifier.hpp
/// \brief Defines the bounded authoritative OPEN source verification seam.

#include "managed_opening_fact_store.hpp"
#include "managed_opening_provenance_contract.hpp"

#include <mt5bridge/dispatch/journal_store.hpp>
#include <mt5bridge/dispatch/lease.hpp>
#include <mt5bridge/reconciliation/graph.hpp>

#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {

/// \class VerifiedManagedOpeningSourceFact
/// \brief Sealed proof that durable OPEN facts match independent broker history.
///
/// This is a source-verification result, not an opening allocation. It cannot
/// be constructed by a caller and does not authorize exposure mutation.
class VerifiedManagedOpeningSourceFact {
public:
    /// \brief Returns the committed OPEN operation identity.
    /// \return Account-scoped operation key.
    const OperationKey &operation_key() const { return operation_key_; }
    /// \brief Returns the exact journal revision verified against the source.
    /// \return Non-zero durable operation revision.
    std::uint64_t operation_revision() const { return operation_revision_; }
    /// \brief Returns the broker position identifier from history deals.
    /// \return Non-zero POSITION_IDENTIFIER.
    std::uint64_t position_identifier() const { return position_identifier_; }
    /// \brief Returns all verified execution deal tickets.
    /// \return Deterministically sorted immutable ticket set.
    const std::vector<std::uint64_t> &deal_tickets() const { return deal_tickets_; }
    /// \brief Returns the cumulative managed quantity from verified facts.
    /// \return Positive logical units.
    std::uint64_t managed_logical_units() const { return managed_logical_units_; }
    /// \brief Returns the canonical source-chain fingerprint.
    /// \return Non-zero deterministic FNV-64 fingerprint over journal,
    /// history, and durable facts; this is not authority by itself.
    std::uint64_t evidence_digest() const { return evidence_digest_; }

    /// \brief Tests the sealed source proof shape.
    /// \return True only for a complete, non-empty proof.
    bool valid() const {
        if (!operation_key_.valid() || operation_revision_ == 0 ||
            position_identifier_ == 0 || deal_tickets_.empty() ||
            managed_logical_units_ == 0 || evidence_digest_ == 0)
            return false;
        for (std::size_t index = 0; index != deal_tickets_.size(); ++index)
            if (deal_tickets_[index] == 0 ||
                (index != 0 && deal_tickets_[index - 1] >= deal_tickets_[index]))
                return false;
        return true;
    }

private:
    VerifiedManagedOpeningSourceFact(OperationKey operation_key,
                                     std::uint64_t operation_revision,
                                     std::uint64_t position_identifier,
                                     std::vector<std::uint64_t> deal_tickets,
                                     std::uint64_t managed_logical_units,
                                     std::uint64_t evidence_digest)
        : operation_key_(std::move(operation_key)),
          operation_revision_(operation_revision),
          position_identifier_(position_identifier),
          deal_tickets_(std::move(deal_tickets)),
          managed_logical_units_(managed_logical_units),
          evidence_digest_(evidence_digest) {}

    OperationKey operation_key_;
    std::uint64_t operation_revision_ = 0;
    std::uint64_t position_identifier_ = 0;
    std::vector<std::uint64_t> deal_tickets_;
    std::uint64_t managed_logical_units_ = 0;
    std::uint64_t evidence_digest_ = 0;

    friend class ManagedOpeningSourceVerifier;
};

/// \enum ManagedOpeningSourceVerificationStatus
/// \brief Reports one independently verified OPEN source result.
enum class ManagedOpeningSourceVerificationStatus : std::uint8_t {
    found = 0,
    invalid_request = 1,
    unavailable = 2,
    invalid_evidence = 3,
    io_error = 4,
    lease_not_held = 5,
};

/// \struct ManagedOpeningSourceVerificationResult
/// \brief Carries a sealed source proof or a fail-closed status.
struct ManagedOpeningSourceVerificationResult {
    ManagedOpeningSourceVerificationStatus status =
        ManagedOpeningSourceVerificationStatus::unavailable;
    std::optional<VerifiedManagedOpeningSourceFact> proof;

    /// \brief Tests whether a complete source proof was returned.
    /// \return True only for `found` and a valid sealed proof.
    bool found() const {
        return status == ManagedOpeningSourceVerificationStatus::found && proof &&
               proof->valid();
    }
};

/// \class ManagedOpeningSourceVerifier
/// \brief Reloads journal, broker history, and durable OPEN facts under fencing.
class ManagedOpeningSourceVerifier final {
public:
    /// \brief Binds the verifier to one durable source set and lease domain.
    /// \param journal_store Durable operation journal.
    /// \param fact_store Durable locally validated OPEN facts.
    /// \param graph Current account-scoped broker observation graph.
    /// \param quantity_domain Reviewed symbol/account quantity mapping.
    /// \param lease Account-scoped writer lease used for continuous ownership.
    ManagedOpeningSourceVerifier(
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const ObservationGraph &graph,
        ManagedOpeningQuantityDomain quantity_domain,
        const SingleWriterLease &lease)
        : journal_store_(journal_store), fact_store_(fact_store), graph_(graph),
          quantity_domain_(quantity_domain), lease_(lease) {}

    /// \brief Verifies one committed managed OPEN from independent sources.
    /// \param request Query constraints; none are treated as proof.
    /// \return Sealed source proof or an explicit fail-closed status.
    ManagedOpeningSourceVerificationResult verify(
        const ManagedOpeningProvenanceRequest &request) const;

private:
    ManagedOpeningSourceVerificationResult verify_locked(
        const ManagedOpeningProvenanceRequest &request) const;

    const DurableJournalStore &journal_store_;
    const DurableManagedOpeningFactStore &fact_store_;
    const ObservationGraph &graph_;
    ManagedOpeningQuantityDomain quantity_domain_;
    const SingleWriterLease &lease_;
};

} // namespace mt5bridge::dispatch

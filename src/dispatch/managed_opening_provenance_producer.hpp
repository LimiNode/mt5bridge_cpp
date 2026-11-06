#pragma once

/// \file dispatch/managed_opening_provenance_producer.hpp
/// \brief Defines the fenced trusted managed-opening proof producer.

#include "managed_opening_allocation.hpp"
#include "managed_opening_fact_store.hpp"
#include "managed_opening_source_verifier.hpp"

#include <mt5bridge/dispatch/journal_store.hpp>
#include <mt5bridge/dispatch/lease.hpp>

namespace mt5bridge::dispatch {

/// \class ManagedOpeningProvenanceProducer
/// \brief Builds sealed opening proofs from independently reloaded sources.
///
/// The producer is deliberately private and read-only. It does not publish an
/// allocation, mutate a journal, wire the owner loop, or change exposure.
class ManagedOpeningProvenanceProducer final
    : public DurableManagedOpeningProvenanceSource {
public:
    /// \brief Binds the producer to the durable source chain and lease domain.
    /// \param journal_store Durable operation journal.
    /// \param fact_store Durable B3A opening facts.
    /// \param allocation_store Durable B3C opening allocations.
    /// \param verifier Fresh B3B source verifier.
    /// \param lease Account-scoped writer lease.
    ManagedOpeningProvenanceProducer(
        const DurableJournalStore &journal_store,
        const DurableManagedOpeningFactStore &fact_store,
        const DurableManagedOpeningAllocationStore &allocation_store,
        const ManagedOpeningSourceVerifier &verifier,
        const SingleWriterLease &lease)
        : journal_store_(journal_store), fact_store_(fact_store),
          allocation_store_(allocation_store), verifier_(verifier), lease_(lease) {}

    /// \brief Rejects the legacy unscoped request path.
    /// \param request Query without an explicit historical/current selector.
    /// \return `invalid_request`; callers must choose a proof scope explicitly.
    ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceRequest &request) const override;

    /// \brief Verifies one explicit historical or current frontier.
    /// \param selector Scope and immutable source constraints.
    /// \return Sealed proof or a fail-closed status.
    ManagedOpeningProvenanceResult verify(
        const ManagedOpeningProvenanceSelector &selector) const override;

private:
    ManagedOpeningProvenanceResult verify_locked(
        const ManagedOpeningProvenanceSelector &selector) const;

    const DurableJournalStore &journal_store_;
    const DurableManagedOpeningFactStore &fact_store_;
    const DurableManagedOpeningAllocationStore &allocation_store_;
    const ManagedOpeningSourceVerifier &verifier_;
    const SingleWriterLease &lease_;
};

} // namespace mt5bridge::dispatch

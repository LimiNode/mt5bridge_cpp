/// \file managed_opening_provenance_producer.cpp
/// \brief Implements the fenced trusted managed-opening proof producer.

#include "managed_opening_provenance_producer.hpp"

#include <algorithm>
#include <cstdint>

namespace mt5bridge::dispatch {
namespace {

enum class FrontierRelation : std::uint8_t { exact, extended, contradiction };

FrontierRelation classify_frontier_relation(
    const ManagedOpeningAllocationEntry &entry,
    const VerifiedManagedOpeningSourceFact &proof) {
    if (!proof.valid() || proof.operation_key() != entry.key.operation_key ||
        proof.operation_revision() != entry.key.operation_revision ||
        proof.position_identifier() != entry.position_identifier)
        return FrontierRelation::contradiction;
    for (const auto &reference : entry.fact_references) {
        if (!std::binary_search(proof.deal_tickets().begin(), proof.deal_tickets().end(),
                                reference.deal_ticket))
            return FrontierRelation::contradiction;
    }
    if (proof.deal_tickets().size() == entry.fact_references.size() &&
        proof.managed_logical_units() == entry.cumulative_managed_logical_units)
        return FrontierRelation::exact;
    if (proof.deal_tickets().size() > entry.fact_references.size() &&
        proof.managed_logical_units() > entry.cumulative_managed_logical_units)
        return FrontierRelation::extended;
    return FrontierRelation::contradiction;
}

ManagedOpeningProvenanceResult map_verifier_result(
    const ManagedOpeningSourceVerificationResult &result) {
    switch (result.status) {
    case ManagedOpeningSourceVerificationStatus::found:
        return result.found()
                   ? ManagedOpeningProvenanceResult{
                         ManagedOpeningProvenanceStatus::found, std::nullopt}
                   : ManagedOpeningProvenanceResult{
                         ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
    case ManagedOpeningSourceVerificationStatus::invalid_request:
        return {ManagedOpeningProvenanceStatus::invalid_request, std::nullopt};
    case ManagedOpeningSourceVerificationStatus::unavailable:
        return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
    case ManagedOpeningSourceVerificationStatus::invalid_evidence:
        return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
    case ManagedOpeningSourceVerificationStatus::io_error:
        return {ManagedOpeningProvenanceStatus::io_error, std::nullopt};
    case ManagedOpeningSourceVerificationStatus::lease_not_held:
        return {ManagedOpeningProvenanceStatus::lease_not_held, std::nullopt};
    }
    return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
}

} // namespace

ManagedOpeningProvenanceResult ManagedOpeningProvenanceProducer::verify(
    const ManagedOpeningProvenanceRequest &request) const {
    (void)request;
    // An unscoped request cannot distinguish historical replay from current
    // frontier admission. Keep the old seam fail-closed until the caller
    // chooses an explicit selector.
    return {ManagedOpeningProvenanceStatus::invalid_request, std::nullopt};
}

ManagedOpeningProvenanceResult ManagedOpeningProvenanceProducer::verify_locked(
    const ManagedOpeningProvenanceSelector &selector) const {
    const auto journal = journal_store_.load(selector.operation_key);
    switch (journal.status) {
    case StoreLoadStatus::not_found:
        return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
    case StoreLoadStatus::invalid_record:
        return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
    case StoreLoadStatus::io_error:
        return {ManagedOpeningProvenanceStatus::io_error, std::nullopt};
    case StoreLoadStatus::found:
        break;
    }
    if (!journal.record || !journal.record->valid() ||
        journal.record->operation_kind != OperationKind::open ||
        journal.record->result_source_revision != selector.operation_revision ||
        journal.record->result_source_revision == 0 ||
        journal.record->journal_state != JournalState::result_persisted ||
        journal.record->result_payload.empty() ||
        (journal.record->operation_state != OperationState::accepted &&
         journal.record->operation_state != OperationState::reconciling) ||
        !journal.record->reconciliation_descriptor)
        return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};

    const auto scanned = allocation_store_.scan(
        selector.operation_key.account, journal_store_, fact_store_);
    if (scanned.status == ManagedOpeningAllocationScanStatus::io_error)
        return {ManagedOpeningProvenanceStatus::io_error, std::nullopt};
    if (!scanned.complete())
        return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};

    std::vector<const ManagedOpeningAllocationEntry *> candidates;
    for (const auto &record : scanned.records) {
        if (record.key.operation_key != selector.operation_key)
            continue;
        if (record.key.operation_revision != selector.operation_revision ||
            (selector.kind == ManagedOpeningProvenanceLookupKind::current &&
             record.position_identifier != selector.position_identifier) ||
            (selector.kind == ManagedOpeningProvenanceLookupKind::exact_historical &&
             selector.position_identifier != 0 &&
             record.position_identifier != selector.position_identifier))
            return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
        candidates.push_back(&record);
    }

    const ManagedOpeningAllocationEntry *selected = nullptr;
    if (selector.kind == ManagedOpeningProvenanceLookupKind::exact_historical) {
        for (const auto *candidate : candidates) {
            if (candidate->key.frontier_revision == selector.frontier_revision) {
                selected = candidate;
                break;
            }
        }
        if (!selected)
            return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
    } else {
        if (candidates.empty())
            return {ManagedOpeningProvenanceStatus::unavailable, std::nullopt};
        if (candidates.size() > 1) {
            // Fresh evidence below distinguishes a compatible historical
            // prefix from two competing current records. Keep the scan
            // complete before making that semantic decision.
        }
    }

    const auto requested_position =
        selector.kind == ManagedOpeningProvenanceLookupKind::exact_historical
            ? selected->position_identifier
            : selector.position_identifier;
    const ManagedOpeningProvenanceRequest fresh_request{
        selector.operation_key, OperationKind::open, journal.record->revision,
        requested_position};
    const auto fresh = verifier_.verify(fresh_request);
    if (!fresh.found())
        return map_verifier_result(fresh);

    if (selector.kind == ManagedOpeningProvenanceLookupKind::exact_historical) {
        const auto relation = classify_frontier_relation(*selected, *fresh.proof);
        if (relation == FrontierRelation::contradiction)
            return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
        return {ManagedOpeningProvenanceStatus::found,
                VerifiedManagedOpeningAllocation(
                    selected->key.operation_key, selected->key.operation_revision,
                    selected->position_identifier, selected->key.frontier_revision,
                    selected->cumulative_managed_logical_units, selected->evidence_digest,
                    ManagedOpeningProofScope::historical)};
    }

    const ManagedOpeningAllocationEntry *current = nullptr;
    bool compatible_prefix = false;
    for (const auto *candidate : candidates) {
        switch (classify_frontier_relation(*candidate, *fresh.proof)) {
        case FrontierRelation::exact:
            if (current)
                return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
            current = candidate;
            break;
        case FrontierRelation::extended:
            compatible_prefix = true;
            break;
        case FrontierRelation::contradiction:
            return {ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
        }
    }
    if (!current)
        return compatible_prefix
                   ? ManagedOpeningProvenanceResult{
                         ManagedOpeningProvenanceStatus::unavailable, std::nullopt}
                   : ManagedOpeningProvenanceResult{
                         ManagedOpeningProvenanceStatus::invalid_evidence, std::nullopt};
    return {ManagedOpeningProvenanceStatus::found,
            VerifiedManagedOpeningAllocation(
                current->key.operation_key, current->key.operation_revision,
                current->position_identifier, current->key.frontier_revision,
                current->cumulative_managed_logical_units, current->evidence_digest,
                ManagedOpeningProofScope::current)};
}

ManagedOpeningProvenanceResult ManagedOpeningProvenanceProducer::verify(
    const ManagedOpeningProvenanceSelector &selector) const {
    if (!selector.valid())
        return {ManagedOpeningProvenanceStatus::invalid_request, std::nullopt};
    const auto token = lease_.held_fencing_token(selector.operation_key.account);
    if (!token)
        return {ManagedOpeningProvenanceStatus::lease_not_held, std::nullopt};

    ManagedOpeningProvenanceResult result{
        ManagedOpeningProvenanceStatus::lease_not_held, std::nullopt};
    const bool fenced = lease_.with_fencing_capability(
        selector.operation_key.account, *token, [&] {
            result = verify_locked(selector);
            if (result.found() &&
                lease_.held_fencing_token(selector.operation_key.account) != token)
                result = {ManagedOpeningProvenanceStatus::lease_not_held, std::nullopt};
        });
    return fenced ? result
                  : ManagedOpeningProvenanceResult{
                        ManagedOpeningProvenanceStatus::lease_not_held, std::nullopt};
}

} // namespace mt5bridge::dispatch

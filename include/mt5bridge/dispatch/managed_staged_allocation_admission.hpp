#pragma once

/// \file dispatch/managed_staged_allocation_admission.hpp
/// \brief Defines source admission for a prepared allocation-gated settlement.

#include "managed_allocation.hpp"
#include "managed_settlement_preparation.hpp"

#include <limits>
#include <optional>

namespace mt5bridge {

/// \enum ManagedStagedAllocationAdmissionStatus
/// \brief Reports staged source admission without mutating any store.
enum class ManagedStagedAllocationAdmissionStatus {
    admitted,
    already_committed,
    invalid_preparation,
    invalid_mapping,
    missing_operation,
    invalid_operation,
    missing_broker,
    invalid_broker,
    missing_envelope,
    invalid_envelope,
    missing_allocation,
    invalid_allocation,
    conflict,
    capacity_exhausted,
    io_error,
};

/// \struct ManagedStagedAllocationAdmissionResult
/// \brief Carries a staged admission decision and canonical mapping.
struct ManagedStagedAllocationAdmissionResult {
    ManagedStagedAllocationAdmissionStatus status =
        ManagedStagedAllocationAdmissionStatus::invalid_preparation;
    std::optional<ManagedBrokerVolumeMapping> mapping;

    /// \brief Tests whether a new staged allocation may be published.
    /// \return True only for an absent allocation key with valid sources.
    bool admitted() const {
        return status == ManagedStagedAllocationAdmissionStatus::admitted &&
               mapping.has_value() && mapping->valid();
    }
};

/// \brief Validates a staged allocation against the exact prepared recipe.
/// \param entry Candidate allocation with an exact frontier reference.
/// \param candidate Mapping derived before the staged publication.
/// \param preparation Durable fact/frontier/target recipe.
/// \param journal_store Durable base-operation source.
/// \param settlement_store Durable historical settlement source.
/// \param broker_store Durable broker reversal source.
/// \param envelope_store Durable broker-envelope source.
/// \param allocation_store Durable allocation ledger for capacity and replay.
/// \return Staged admission result; no source is mutated.
inline ManagedStagedAllocationAdmissionResult admit_staged_managed_allocation(
    const ManagedAllocationEntry &entry, const ManagedBrokerVolumeMapping &candidate,
    const ManagedSettlementPreparation &preparation,
    const DurableManagedSettlementPreparationStore &preparation_store,
    const DurableJournalStore &journal_store,
    const DurableManagedDealSettlementStore &settlement_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store,
    const DurableManagedAllocationStore &allocation_store) {
    if (!preparation.valid())
        return {ManagedStagedAllocationAdmissionStatus::invalid_preparation, std::nullopt};
    const auto durable_preparation = preparation_store.load(
        preparation.target_operation.key, preparation.target_operation.revision,
        preparation.fact.key.deal_ticket);
    if (durable_preparation.status == ManagedSettlementPreparationLoadStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (!durable_preparation.found() || !(*durable_preparation.record == preparation))
        return {ManagedStagedAllocationAdmissionStatus::invalid_preparation, std::nullopt};
    if (!entry.valid() || !entry.settlement_frontier_digest ||
        entry.settlement_frontier_revision != preparation.frontier.operation_revision ||
        *entry.settlement_frontier_digest !=
            managed_settlement_frontier_digest(preparation.frontier) ||
        !candidate.valid() || !(entry.account == preparation.fact.key.account) ||
        !(entry.allocation_key.managed_key == preparation.fact.key) ||
        !(entry.allocation_key.broker_key ==
          preparation.staged_ownership.basis_key.broker_key) ||
        entry.settlement_source_operation_revision !=
            preparation.fact.source_operation_revision ||
        entry.basis_operation_revision != preparation.target_operation.revision)
        return {ManagedStagedAllocationAdmissionStatus::invalid_mapping, std::nullopt};

    const auto operation = journal_store.load(preparation.target_operation.key);
    if (operation.status == StoreLoadStatus::not_found)
        return {ManagedStagedAllocationAdmissionStatus::missing_operation, std::nullopt};
    if (operation.status == StoreLoadStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (operation.status != StoreLoadStatus::found || !operation.record ||
        !operation.record->valid())
        return {ManagedStagedAllocationAdmissionStatus::invalid_operation, std::nullopt};
    if (!(operation.record->key == preparation.target_operation.key) ||
        operation.record->revision != preparation.base_operation_revision)
        return {ManagedStagedAllocationAdmissionStatus::invalid_operation, std::nullopt};

    const auto broker = broker_store.load(entry.allocation_key.broker_key);
    if (broker.status == BrokerReversalLoadStatus::not_found)
        return {ManagedStagedAllocationAdmissionStatus::missing_broker, std::nullopt};
    if (broker.status == BrokerReversalLoadStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (broker.status != BrokerReversalLoadStatus::found || !broker.record ||
        !broker.record->valid() ||
        !(preparation.staged_ownership.broker_provenance == broker.record->provenance))
        return {ManagedStagedAllocationAdmissionStatus::invalid_broker, std::nullopt};
    if (broker.record->deal_entry == BrokerDealEntry::inout)
        return {ManagedStagedAllocationAdmissionStatus::invalid_broker, std::nullopt};

    const auto envelope = envelope_store.load(entry.allocation_key.broker_key);
    if (envelope.status == BrokerAllocationLoadStatus::not_found)
        return {ManagedStagedAllocationAdmissionStatus::missing_envelope, std::nullopt};
    if (envelope.status == BrokerAllocationLoadStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (envelope.status != BrokerAllocationLoadStatus::found || !envelope.record ||
        !envelope.record->valid() ||
        !(envelope.record->broker_provenance == broker.record->provenance) ||
        !(envelope.record->broker_close_leg == broker.record->broker_close_leg) ||
        !(envelope.record->broker_reverse_open_leg ==
          broker.record->broker_reverse_open_leg) ||
        !(envelope.record->unallocated_close_leg == broker.record->broker_close_leg) ||
        !(envelope.record->unallocated_reverse_open_leg ==
          broker.record->broker_reverse_open_leg))
        return {ManagedStagedAllocationAdmissionStatus::invalid_envelope, std::nullopt};

    const auto derived = derive_managed_broker_volume_mapping(
        preparation.fact, *broker.record);
    if (!derived || !(*derived == candidate) ||
        !(entry.account == candidate.managed_key().account) ||
        !(entry.allocation_key.broker_key == candidate.broker_key()) ||
        !(entry.allocation_key.managed_key == candidate.managed_key()) ||
        !(entry.settlement_provenance == candidate.managed_provenance()) ||
        !(entry.broker_provenance == candidate.broker_provenance()) ||
        entry.managed_logical_units != candidate.managed_logical_units() ||
        !(entry.broker_volume == candidate.broker_volume()))
        return {ManagedStagedAllocationAdmissionStatus::invalid_mapping, std::nullopt};

    const auto existing = allocation_store.load(entry.allocation_key);
    if (existing.status == ManagedAllocationLoadStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (existing.status == ManagedAllocationLoadStatus::invalid_record)
        return {ManagedStagedAllocationAdmissionStatus::invalid_allocation, std::nullopt};
    if (existing.status == ManagedAllocationLoadStatus::found && existing.record) {
        return *existing.record == entry
                   ? ManagedStagedAllocationAdmissionResult{
                         ManagedStagedAllocationAdmissionStatus::already_committed, candidate}
                   : ManagedStagedAllocationAdmissionResult{
                         ManagedStagedAllocationAdmissionStatus::conflict, std::nullopt};
    }

    const auto historical = allocation_store.scan_recovery(
        settlement_store, broker_store, basis_store, envelope_store, preparation_store);
    if (historical.status == ManagedAllocationScanStatus::io_error)
        return {ManagedStagedAllocationAdmissionStatus::io_error, std::nullopt};
    if (historical.status != ManagedAllocationScanStatus::complete)
        return {ManagedStagedAllocationAdmissionStatus::invalid_allocation, std::nullopt};

    BrokerVolume total{};
    for (const auto &record : historical.records) {
        if (!(record.allocation_key.broker_key == entry.allocation_key.broker_key))
            continue;
        if (!total.valid())
            total = record.broker_volume;
        else if (total.scale != record.broker_volume.scale ||
                 total.step_units != record.broker_volume.step_units ||
                 total.units > (std::numeric_limits<std::uint64_t>::max)() -
                                   record.broker_volume.units)
            return {ManagedStagedAllocationAdmissionStatus::invalid_allocation, std::nullopt};
        else
            total.units += record.broker_volume.units;
    }
    if (total.valid() &&
        (total.scale != entry.broker_volume.scale ||
         total.step_units != entry.broker_volume.step_units ||
         total.units > (std::numeric_limits<std::uint64_t>::max)() -
                            entry.broker_volume.units))
        return {ManagedStagedAllocationAdmissionStatus::capacity_exhausted, std::nullopt};
    const auto used = total.valid() ? total.units : 0;
    if (used > envelope.record->broker_close_leg.units ||
        entry.broker_volume.units > envelope.record->broker_close_leg.units - used)
        return {ManagedStagedAllocationAdmissionStatus::capacity_exhausted, std::nullopt};
    return {ManagedStagedAllocationAdmissionStatus::admitted, candidate};
}

} // namespace mt5bridge

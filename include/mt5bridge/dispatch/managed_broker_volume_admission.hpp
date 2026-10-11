#pragma once

/// \file dispatch/managed_broker_volume_admission.hpp
/// \brief Defines the durable source-coherence gate for broker-volume mappings.

#include "broker_allocation_envelope.hpp"
#include "managed_broker_volume_mapping.hpp"
#include "managed_ownership_basis.hpp"

#include <optional>

namespace mt5bridge {

/// \enum ManagedBrokerVolumeAdmissionStatus
/// \brief Reports why a mapping did or did not pass durable source admission.
enum class ManagedBrokerVolumeAdmissionStatus {
    admitted,                    ///< Durable source proofs are coherent.
    invalid_mapping,             ///< The caller-supplied mapping is malformed.
    missing_settlement_fact,     ///< The immutable managed fact is absent.
    invalid_settlement_fact,     ///< The managed fact is malformed or inconsistent.
    settlement_mismatch,         ///< The durable fact differs from the candidate.
    missing_broker_record,       ///< The durable broker reversal is absent.
    invalid_broker_record,       ///< The durable broker reversal is malformed.
    broker_mismatch,             ///< The durable broker proof differs from the candidate.
    mapping_mismatch,            ///< Re-derivation from durable proofs differs from candidate.
    missing_operation_record,    ///< The bound operation is absent.
    invalid_operation_record,    ///< The bound operation cannot authorize settlement.
    missing_ownership_basis,     ///< No durable operation/reversal association exists.
    invalid_ownership_basis,     ///< The durable ownership basis is malformed or mismatched.
    missing_allocation_envelope, ///< No broker-only envelope exists for the reversal.
    invalid_allocation_envelope, ///< The broker-only envelope is malformed or consumed.
    io_error,                    ///< A durable source could not complete its read.
};

/// \struct ManagedBrokerVolumeAdmissionResult
/// \brief Carries a source-coherent mapping admission decision.
struct ManagedBrokerVolumeAdmissionResult {
    ManagedBrokerVolumeAdmissionStatus status =
        ManagedBrokerVolumeAdmissionStatus::invalid_mapping; ///< Admission classification.
    std::optional<ManagedBrokerVolumeMapping> mapping; ///< Canonical durable mapping.

    /// \brief Tests whether durable sources admitted this mapping candidate.
    /// \return True only when a canonical mapping is present.
    bool admitted() const {
        return status == ManagedBrokerVolumeAdmissionStatus::admitted &&
               mapping.has_value() && mapping->valid();
    }
};

namespace managed_broker_volume_admission_detail {

/// \brief Converts a settlement-store load status to an admission status.
/// \param status Durable settlement lookup status.
/// \return The corresponding fail-closed admission classification.
inline ManagedBrokerVolumeAdmissionStatus settlement_load_status(
    ManagedDealSettlementLoadStatus status) {
    switch (status) {
    case ManagedDealSettlementLoadStatus::not_found:
        return ManagedBrokerVolumeAdmissionStatus::missing_settlement_fact;
    case ManagedDealSettlementLoadStatus::invalid_record:
        return ManagedBrokerVolumeAdmissionStatus::invalid_settlement_fact;
    case ManagedDealSettlementLoadStatus::io_error:
        return ManagedBrokerVolumeAdmissionStatus::io_error;
    case ManagedDealSettlementLoadStatus::found:
        return ManagedBrokerVolumeAdmissionStatus::invalid_settlement_fact;
    }
    return ManagedBrokerVolumeAdmissionStatus::io_error;
}

/// \brief Converts a broker-store load status to an admission status.
/// \param status Durable broker lookup status.
/// \return The corresponding fail-closed admission classification.
inline ManagedBrokerVolumeAdmissionStatus broker_load_status(
    BrokerReversalLoadStatus status) {
    switch (status) {
    case BrokerReversalLoadStatus::not_found:
        return ManagedBrokerVolumeAdmissionStatus::missing_broker_record;
    case BrokerReversalLoadStatus::invalid_record:
        return ManagedBrokerVolumeAdmissionStatus::invalid_broker_record;
    case BrokerReversalLoadStatus::io_error:
        return ManagedBrokerVolumeAdmissionStatus::io_error;
    case BrokerReversalLoadStatus::found:
        return ManagedBrokerVolumeAdmissionStatus::invalid_broker_record;
    }
    return ManagedBrokerVolumeAdmissionStatus::io_error;
}

/// \brief Converts an ownership-basis load status to an admission status.
/// \param status Durable basis lookup status.
/// \return The corresponding fail-closed admission classification.
inline ManagedBrokerVolumeAdmissionStatus basis_load_status(
    ManagedOwnershipBasisLoadStatus status) {
    switch (status) {
    case ManagedOwnershipBasisLoadStatus::not_found:
        return ManagedBrokerVolumeAdmissionStatus::missing_ownership_basis;
    case ManagedOwnershipBasisLoadStatus::invalid_record:
        return ManagedBrokerVolumeAdmissionStatus::invalid_ownership_basis;
    case ManagedOwnershipBasisLoadStatus::io_error:
        return ManagedBrokerVolumeAdmissionStatus::io_error;
    case ManagedOwnershipBasisLoadStatus::found:
        return ManagedBrokerVolumeAdmissionStatus::invalid_ownership_basis;
    }
    return ManagedBrokerVolumeAdmissionStatus::io_error;
}

/// \brief Converts ownership-basis proof status to an admission status.
/// \param status Result of validating the operation and envelope sources.
/// \return The corresponding admission classification.
inline ManagedBrokerVolumeAdmissionStatus basis_proof_status(
    ManagedOwnershipBasisProofStatus status) {
    switch (status) {
    case ManagedOwnershipBasisProofStatus::valid:
        return ManagedBrokerVolumeAdmissionStatus::admitted;
    case ManagedOwnershipBasisProofStatus::invalid_record:
        return ManagedBrokerVolumeAdmissionStatus::invalid_ownership_basis;
    case ManagedOwnershipBasisProofStatus::missing_operation_record:
        return ManagedBrokerVolumeAdmissionStatus::missing_operation_record;
    case ManagedOwnershipBasisProofStatus::invalid_operation_record:
        return ManagedBrokerVolumeAdmissionStatus::invalid_operation_record;
    case ManagedOwnershipBasisProofStatus::missing_allocation_envelope:
        return ManagedBrokerVolumeAdmissionStatus::missing_allocation_envelope;
    case ManagedOwnershipBasisProofStatus::invalid_allocation_envelope:
        return ManagedBrokerVolumeAdmissionStatus::invalid_allocation_envelope;
    case ManagedOwnershipBasisProofStatus::io_error:
        return ManagedBrokerVolumeAdmissionStatus::io_error;
    }
    return ManagedBrokerVolumeAdmissionStatus::io_error;
}

} // namespace managed_broker_volume_admission_detail

/// \brief Revalidates a mapping against all durable source proofs.
/// \param candidate Mapping produced from an earlier reconciliation pass.
/// \param journal_store Durable operation records used by the ownership basis.
/// \param settlement_store Durable immutable managed per-deal facts.
/// \param broker_store Durable broker reversal proofs.
/// \param basis_store Durable operation-to-broker ownership associations.
/// \param allocation_store Durable broker-only unallocated envelopes.
/// \return A canonical mapping copied from durable proofs, or a fail-closed status.
///
/// This is a read-only source-coherence gate. It does not reserve either broker
/// leg, mutate managed exposure, choose an allocation policy, or prevent a
/// second concurrent caller from making the same candidate. Those guarantees
/// require the later durable allocation ledger.
inline ManagedBrokerVolumeAdmissionResult admit_managed_broker_volume_mapping(
    const ManagedBrokerVolumeMapping &candidate,
    const DurableJournalStore &journal_store,
    const DurableManagedDealSettlementStore &settlement_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &allocation_store) {
    using namespace managed_broker_volume_admission_detail;

    if (!candidate.valid())
        return {ManagedBrokerVolumeAdmissionStatus::invalid_mapping, std::nullopt};

    const auto settlement = settlement_store.load(candidate.managed_key());
    if (settlement.status != ManagedDealSettlementLoadStatus::found ||
        !settlement.record || !settlement.record->valid())
        return {settlement_load_status(settlement.status), std::nullopt};
    if (!(settlement.record->key == candidate.managed_key()) ||
        !(settlement.record->provenance == candidate.managed_provenance()) ||
        settlement.record->managed_logical_units != candidate.managed_logical_units())
        return {ManagedBrokerVolumeAdmissionStatus::settlement_mismatch, std::nullopt};

    const auto broker = broker_store.load(candidate.broker_key());
    if (broker.status != BrokerReversalLoadStatus::found || !broker.record ||
        !broker.record->valid())
        return {broker_load_status(broker.status), std::nullopt};
    if (!(broker.record->key() == candidate.broker_key()) ||
        broker.record->account != candidate.managed_key().account ||
        !(broker.record->provenance == candidate.broker_provenance()) ||
        broker.record->deal_entry != BrokerDealEntry::inout)
        return {ManagedBrokerVolumeAdmissionStatus::broker_mismatch, std::nullopt};

    const auto derived = derive_managed_broker_volume_mapping(
        *settlement.record, *broker.record);
    if (!derived || !(*derived == candidate))
        return {ManagedBrokerVolumeAdmissionStatus::mapping_mismatch, std::nullopt};

    const ManagedOwnershipBasisKey basis_key{
        candidate.broker_key(), candidate.managed_key().operation_key};
    const auto basis = basis_store.load(basis_key);
    if (basis.status != ManagedOwnershipBasisLoadStatus::found || !basis.record ||
        !basis.record->valid())
        return {basis_load_status(basis.status), std::nullopt};
    if (basis.record->account != candidate.managed_key().account ||
        !(basis.record->key() == basis_key) ||
        !(basis.record->broker_provenance == candidate.broker_provenance()) ||
        basis.record->operation_revision != settlement.record->source_operation_revision)
        return {ManagedBrokerVolumeAdmissionStatus::invalid_ownership_basis, std::nullopt};

    const auto envelope = allocation_store.load(candidate.broker_key());
    if (envelope.status == BrokerAllocationLoadStatus::not_found)
        return {ManagedBrokerVolumeAdmissionStatus::missing_allocation_envelope,
                std::nullopt};
    if (envelope.status == BrokerAllocationLoadStatus::invalid_record)
        return {ManagedBrokerVolumeAdmissionStatus::invalid_allocation_envelope,
                std::nullopt};
    if (envelope.status == BrokerAllocationLoadStatus::io_error)
        return {ManagedBrokerVolumeAdmissionStatus::io_error, std::nullopt};
    if (!envelope.found() || !envelope.record || !envelope.record->valid() ||
        envelope.record->account != candidate.managed_key().account ||
        !(envelope.record->key() == candidate.broker_key()) ||
        !(envelope.record->broker_provenance == candidate.broker_provenance()) ||
        !(envelope.record->broker_close_leg == broker.record->broker_close_leg) ||
        !(envelope.record->broker_reverse_open_leg ==
          broker.record->broker_reverse_open_leg) ||
        !(envelope.record->unallocated_close_leg == broker.record->broker_close_leg) ||
        !(envelope.record->unallocated_reverse_open_leg ==
          broker.record->broker_reverse_open_leg))
        return {ManagedBrokerVolumeAdmissionStatus::invalid_allocation_envelope,
                std::nullopt};

    const auto operation = journal_store.load(candidate.managed_key().operation_key);
    if (operation.status == StoreLoadStatus::not_found)
        return {ManagedBrokerVolumeAdmissionStatus::missing_operation_record,
                std::nullopt};
    if (operation.status == StoreLoadStatus::io_error)
        return {ManagedBrokerVolumeAdmissionStatus::io_error, std::nullopt};
    if (operation.status == StoreLoadStatus::invalid_record || !operation.record ||
        !operation.record->valid())
        return {ManagedBrokerVolumeAdmissionStatus::invalid_operation_record,
                std::nullopt};
    if (!(operation.record->key == candidate.managed_key().operation_key) ||
        operation.record->revision != settlement.record->source_operation_revision ||
        settlement.record->managed_logical_units > operation.record->settled_volume)
        return {ManagedBrokerVolumeAdmissionStatus::invalid_operation_record,
                std::nullopt};

    const auto basis_proof = validate_managed_ownership_basis(
        *basis.record, journal_store, allocation_store);
    const auto basis_status = basis_proof_status(basis_proof);
    if (basis_status != ManagedBrokerVolumeAdmissionStatus::admitted)
        return {basis_status, std::nullopt};

    return {ManagedBrokerVolumeAdmissionStatus::admitted, derived};
}

} // namespace mt5bridge

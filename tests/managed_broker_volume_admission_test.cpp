/// \file managed_broker_volume_admission_test.cpp
/// \brief Exercises durable source-coherence admission for broker mappings.

#include <mt5bridge/dispatch/managed_broker_volume_admission.hpp>

#include "support/memory_journal_store.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::ManagedSettlementProvenance managed_provenance() {
    return {11, 8, 8, 101};
}

mt5bridge::BrokerReversalProvenance broker_provenance() {
    return {11, 8, 12, 8, 12, 10, 202};
}

mt5bridge::OperationRecord operation() {
    const mt5bridge::OperationKey key{account(), 71, 4};
    const auto baseline = mt5bridge::ReconciliationBaseline::restore(
        key.account, 4, 7, 1, 2, 3, 7);
    require(baseline.has_value(), "operation baseline is invalid");

    mt5bridge::ReconciliationDescriptor descriptor{
        key.account,
        *baseline,
        {mt5bridge::require_history_deal(
            701, mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = 2;

    mt5bridge::OperationRecord result;
    result.key = key;
    result.operation_kind = mt5bridge::OperationKind::close;
    result.request_payload = {0x01};
    result.result_payload = {0x02};
    result.operation_state = mt5bridge::OperationState::filled;
    result.journal_state = mt5bridge::JournalState::reconciling;
    result.revision = 8;
    result.fencing_token = 3;
    result.reconciliation_descriptor = std::move(descriptor);
    result.settled_volume = 2;
    require(result.valid(), "operation fixture is invalid");
    return result;
}

mt5bridge::ManagedDealSettlement settlement() {
    mt5bridge::ManagedDealSettlement result;
    result.key = {account(), operation().key, 701};
    result.source_operation_revision = 8;
    result.managed_logical_units = 2;
    result.provenance = managed_provenance();
    require(result.valid(), "managed settlement fixture is invalid");
    return result;
}

mt5bridge::BrokerReversalRecord broker() {
    mt5bridge::BrokerReversalRecord result;
    result.account = account();
    result.margin_mode = mt5bridge::BrokerMarginMode::retail_netting;
    result.symbol = "EURUSD";
    result.deal_ticket = 701;
    result.order_ticket = 702;
    result.deal_position_id = 800;
    result.deal_entry = mt5bridge::BrokerDealEntry::inout;
    result.pre_position_identifier = 800;
    result.pre_direction = mt5bridge::BrokerPositionDirection::buy;
    result.pre_volume = {500, 2, 1};
    result.post_position_identifier = 800;
    result.post_direction = mt5bridge::BrokerPositionDirection::sell;
    result.post_volume = {100, 2, 1};
    result.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    result.deal_volume = {600, 2, 1};
    result.broker_close_leg = result.pre_volume;
    result.broker_reverse_open_leg = result.post_volume;
    result.provenance = broker_provenance();
    require(result.valid(), "broker reversal fixture is invalid");
    return result;
}

mt5bridge::BrokerAllocationEnvelope envelope(
    const mt5bridge::BrokerReversalRecord &source) {
    mt5bridge::BrokerAllocationEnvelope result;
    result.account = source.account;
    result.broker_key = source.key();
    result.broker_provenance = source.provenance;
    result.broker_close_leg = source.broker_close_leg;
    result.broker_reverse_open_leg = source.broker_reverse_open_leg;
    result.unallocated_close_leg = source.broker_close_leg;
    result.unallocated_reverse_open_leg = source.broker_reverse_open_leg;
    require(result.valid(), "allocation envelope fixture is invalid");
    return result;
}

mt5bridge::ManagedOwnershipBasis basis(
    const mt5bridge::BrokerReversalRecord &source) {
    mt5bridge::ManagedOwnershipBasis result;
    result.account = source.account;
    result.basis_key = {source.key(), operation().key};
    result.broker_provenance = source.provenance;
    result.operation_revision = 8;
    require(result.valid(), "ownership basis fixture is invalid");
    return result;
}

mt5bridge::ManagedBrokerVolumeMapping make_mapping() {
    const auto result = mt5bridge::derive_managed_broker_volume_mapping(
        settlement(), broker());
    require(result.has_value(), "managed broker mapping fixture is invalid");
    return *result;
}

class MemorySettlementStore final : public mt5bridge::DurableManagedDealSettlementStore {
public:
    mt5bridge::ManagedDealSettlementCommitStatus commit(
        const mt5bridge::ManagedDealSettlementCommit &,
        const mt5bridge::DurableJournalStore &) override {
        return mt5bridge::ManagedDealSettlementCommitStatus::io_error;
    }

    mt5bridge::ManagedDealSettlementLoadResult load(
        const mt5bridge::ManagedDealSettlementKey &key) const override {
        if (load_status != mt5bridge::ManagedDealSettlementLoadStatus::found)
            return {load_status, std::nullopt};
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::ManagedDealSettlementLoadResult{
                         mt5bridge::ManagedDealSettlementLoadStatus::not_found, std::nullopt}
                   : mt5bridge::ManagedDealSettlementLoadResult{
                         mt5bridge::ManagedDealSettlementLoadStatus::found, found->second};
    }

    mt5bridge::ManagedDealSettlementLoadStatus load_status =
        mt5bridge::ManagedDealSettlementLoadStatus::found;
    std::map<mt5bridge::ManagedDealSettlementKey, mt5bridge::ManagedDealSettlement> records;
};

class MemoryBrokerStore final : public mt5bridge::DurableBrokerReversalStore {
public:
    mt5bridge::BrokerReversalCommitStatus commit(
        const mt5bridge::BrokerReversalRecord &) override {
        return mt5bridge::BrokerReversalCommitStatus::io_error;
    }

    mt5bridge::BrokerReversalLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (load_status != mt5bridge::BrokerReversalLoadStatus::found)
            return {load_status, std::nullopt};
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt}
                   : mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::found, found->second};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        mt5bridge::BrokerReversalScanResult result;
        result.status = mt5bridge::BrokerReversalScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    mt5bridge::BrokerReversalLoadStatus load_status =
        mt5bridge::BrokerReversalLoadStatus::found;
    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerReversalRecord> records;
};

class MemoryEnvelopeStore final : public mt5bridge::DurableBrokerAllocationStore {
public:
    mt5bridge::BrokerAllocationCommitStatus commit(
        const mt5bridge::BrokerAllocationEnvelope &,
        const mt5bridge::DurableBrokerReversalStore &) override {
        return mt5bridge::BrokerAllocationCommitStatus::io_error;
    }

    mt5bridge::BrokerAllocationLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (load_status != mt5bridge::BrokerAllocationLoadStatus::found)
            return {load_status, std::nullopt};
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::not_found, std::nullopt}
                   : mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::found, found->second};
    }

    mt5bridge::BrokerAllocationScanResult scan() const override {
        mt5bridge::BrokerAllocationScanResult result;
        result.status = mt5bridge::BrokerAllocationScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    mt5bridge::BrokerAllocationLoadStatus load_status =
        mt5bridge::BrokerAllocationLoadStatus::found;
    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerAllocationEnvelope> records;
};

class MemoryBasisStore final : public mt5bridge::DurableManagedOwnershipBasisStore {
public:
    mt5bridge::ManagedOwnershipBasisCommitStatus commit(
        const mt5bridge::ManagedOwnershipBasis &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableBrokerAllocationStore &) override {
        return mt5bridge::ManagedOwnershipBasisCommitStatus::io_error;
    }

    mt5bridge::ManagedOwnershipBasisLoadResult load(
        const mt5bridge::ManagedOwnershipBasisKey &key) const override {
        if (load_status != mt5bridge::ManagedOwnershipBasisLoadStatus::found)
            return {load_status, std::nullopt};
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::ManagedOwnershipBasisLoadResult{
                         mt5bridge::ManagedOwnershipBasisLoadStatus::not_found, std::nullopt}
                   : mt5bridge::ManagedOwnershipBasisLoadResult{
                         mt5bridge::ManagedOwnershipBasisLoadStatus::found, found->second};
    }

    mt5bridge::ManagedOwnershipBasisScanResult scan() const override {
        mt5bridge::ManagedOwnershipBasisScanResult result;
        result.status = mt5bridge::ManagedOwnershipBasisScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    mt5bridge::ManagedOwnershipBasisLoadStatus load_status =
        mt5bridge::ManagedOwnershipBasisLoadStatus::found;
    std::map<mt5bridge::ManagedOwnershipBasisKey, mt5bridge::ManagedOwnershipBasis> records;
};

struct Fixture {
    mt5bridge_test_support::MemoryJournalStore journal;
    MemorySettlementStore settlements;
    MemoryBrokerStore brokers;
    MemoryBasisStore bases;
    MemoryEnvelopeStore envelopes;
    mt5bridge::ManagedBrokerVolumeMapping mapping;

    Fixture() : mapping(make_mapping()) {
        const auto source_operation = operation();
        const auto source_settlement = settlement();
        const auto source_broker = broker();
        journal.records.emplace(source_operation.key, source_operation);
        settlements.records.emplace(source_settlement.key, source_settlement);
        brokers.records.emplace(source_broker.key(), source_broker);
        bases.records.emplace(basis(source_broker).key(), basis(source_broker));
        envelopes.records.emplace(source_broker.key(), envelope(source_broker));
    }
};

void test_admission_from_durable_sources() {
    Fixture fixture;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.admitted(), "coherent durable mapping was not admitted");
    require(result.mapping && *result.mapping == fixture.mapping,
            "admission did not return the canonical mapping");
}

void test_candidate_cannot_bypass_durable_fact() {
    Fixture fixture;
    auto altered = fixture.settlements.records.begin()->second;
    altered.managed_logical_units = 1;
    fixture.settlements.records.begin()->second = altered;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.status == mt5bridge::ManagedBrokerVolumeAdmissionStatus::settlement_mismatch,
            "caller mapping bypassed the durable settlement fact");
}

void test_missing_basis_fails_closed() {
    Fixture fixture;
    fixture.bases.records.clear();
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.status == mt5bridge::ManagedBrokerVolumeAdmissionStatus::missing_ownership_basis,
            "missing ownership basis was admitted");
}

void test_consumed_envelope_fails_closed() {
    Fixture fixture;
    auto consumed = fixture.envelopes.records.begin()->second;
    consumed.unallocated_close_leg.units = 100;
    fixture.envelopes.records.begin()->second = consumed;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.status == mt5bridge::ManagedBrokerVolumeAdmissionStatus::invalid_allocation_envelope,
            "consumed broker envelope was admitted");
}

void test_envelope_legs_must_match_broker_proof() {
    Fixture fixture;
    auto foreign_legs = fixture.envelopes.records.begin()->second;
    foreign_legs.broker_close_leg.units = 400;
    foreign_legs.unallocated_close_leg = foreign_legs.broker_close_leg;
    fixture.envelopes.records.begin()->second = foreign_legs;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.status == mt5bridge::ManagedBrokerVolumeAdmissionStatus::invalid_allocation_envelope,
            "envelope with foreign broker legs was admitted");
}

void test_operation_revision_mismatch_fails_closed() {
    Fixture fixture;
    auto stale = fixture.journal.records.begin()->second;
    stale.revision = 9;
    fixture.journal.records[stale.key] = stale;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.status == mt5bridge::ManagedBrokerVolumeAdmissionStatus::invalid_operation_record,
            "operation revision mismatch was admitted");
}

void test_older_fact_revision_is_valid_for_later_basis() {
    Fixture fixture;
    auto older_fact = fixture.settlements.records.begin()->second;
    older_fact.source_operation_revision = 7;
    fixture.settlements.records.begin()->second = older_fact;
    const auto result = mt5bridge::admit_managed_broker_volume_mapping(
        fixture.mapping, fixture.journal, fixture.settlements, fixture.brokers,
        fixture.bases, fixture.envelopes);
    require(result.admitted(),
            "an immutable earlier fact was rejected by a later basis revision");
}

} // namespace

int main() {
    try {
        test_admission_from_durable_sources();
        test_candidate_cannot_bypass_durable_fact();
        test_missing_basis_fails_closed();
        test_consumed_envelope_fails_closed();
        test_envelope_legs_must_match_broker_proof();
        test_operation_revision_mismatch_fails_closed();
        test_older_fact_revision_is_valid_for_later_basis();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "managed broker volume admission tests passed\n";
    return 0;
}

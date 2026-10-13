/// \file managed_allocation_store_test.cpp
/// \brief Exercises the durable managed close-leg allocation ledger.

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>

#include "support/memory_journal_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::BrokerReversalProvenance broker_provenance() {
    return {11, 8, 12, 8, 12, 10, 202};
}

mt5bridge::ManagedSettlementProvenance settlement_provenance() {
    return {11, 8, 8, 101};
}

mt5bridge::OperationRecord operation(const mt5bridge::OperationKey &key,
                                      std::uint64_t revision,
                                      std::uint64_t deal_ticket) {
    const auto baseline = mt5bridge::ReconciliationBaseline::restore(
        key.account, 4, 7, 1, 2, 3, 7);
    require(baseline.has_value(), "operation baseline is invalid");

    mt5bridge::ReconciliationDescriptor descriptor{
        key.account,
        *baseline,
        {mt5bridge::require_history_deal(
            deal_ticket, mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::partially_filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = 5;

    mt5bridge::OperationRecord result;
    result.key = key;
    result.operation_kind = mt5bridge::OperationKind::close;
    result.request_payload = {0x01};
    result.result_payload = {0x02};
    result.operation_state = mt5bridge::OperationState::partially_filled;
    result.journal_state = mt5bridge::JournalState::reconciling;
    result.revision = revision;
    result.fencing_token = 3;
    result.reconciliation_descriptor = std::move(descriptor);
    result.settled_volume = 2;
    require(result.valid(), "operation fixture is invalid");
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
    result.pre_volume = {600, 2, 1};
    result.post_position_identifier = 800;
    result.post_direction = mt5bridge::BrokerPositionDirection::sell;
    result.post_volume = {100, 2, 1};
    result.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    result.deal_volume = {700, 2, 1};
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
    require(result.valid(), "broker envelope fixture is invalid");
    return result;
}

class MemorySettlementStore final
    : public mt5bridge::DurableManagedDealSettlementStore {
public:
    mt5bridge::ManagedDealSettlementCommitStatus commit(
        const mt5bridge::ManagedDealSettlementCommit &,
        const mt5bridge::DurableJournalStore &) override {
        return mt5bridge::ManagedDealSettlementCommitStatus::io_error;
    }

    mt5bridge::ManagedDealSettlementLoadResult load(
        const mt5bridge::ManagedDealSettlementKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::ManagedDealSettlementLoadResult{
                         mt5bridge::ManagedDealSettlementLoadStatus::not_found,
                         std::nullopt}
                   : mt5bridge::ManagedDealSettlementLoadResult{
                         mt5bridge::ManagedDealSettlementLoadStatus::found,
                         found->second};
    }

    std::map<mt5bridge::ManagedDealSettlementKey,
             mt5bridge::ManagedDealSettlement>
        records;
};

class MemoryBrokerStore final : public mt5bridge::DurableBrokerReversalStore {
public:
    mt5bridge::BrokerReversalCommitStatus commit(
        const mt5bridge::BrokerReversalRecord &) override {
        return mt5bridge::BrokerReversalCommitStatus::io_error;
    }

    mt5bridge::BrokerReversalLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::not_found,
                         std::nullopt}
                   : mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::found,
                         found->second};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        mt5bridge::BrokerReversalScanResult result;
        result.status = mt5bridge::BrokerReversalScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerReversalRecord>
        records;
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
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::not_found,
                         std::nullopt}
                   : mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::found,
                         found->second};
    }

    mt5bridge::BrokerAllocationScanResult scan() const override {
        mt5bridge::BrokerAllocationScanResult result;
        result.status = mt5bridge::BrokerAllocationScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerAllocationEnvelope>
        records;
};

class MemoryBasisStore final
    : public mt5bridge::DurableManagedOwnershipBasisStore {
public:
    mt5bridge::ManagedOwnershipBasisCommitStatus commit(
        const mt5bridge::ManagedOwnershipBasis &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableBrokerAllocationStore &) override {
        return mt5bridge::ManagedOwnershipBasisCommitStatus::io_error;
    }

    mt5bridge::ManagedOwnershipBasisLoadResult load(
        const mt5bridge::ManagedOwnershipBasisKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::ManagedOwnershipBasisLoadResult{
                         mt5bridge::ManagedOwnershipBasisLoadStatus::not_found,
                         std::nullopt}
                   : mt5bridge::ManagedOwnershipBasisLoadResult{
                         mt5bridge::ManagedOwnershipBasisLoadStatus::found,
                         found->second};
    }

    mt5bridge::ManagedOwnershipBasisScanResult scan() const override {
        mt5bridge::ManagedOwnershipBasisScanResult result;
        result.status = mt5bridge::ManagedOwnershipBasisScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::ManagedOwnershipBasisKey,
             mt5bridge::ManagedOwnershipBasis>
        records;
};

class TestLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &) const override {
        ++calls;
        if (lose_on_call != 0 && calls >= lose_on_call)
            return std::nullopt;
        return token;
    }

    bool with_fencing_capability(
        const mt5bridge::AccountKey &account, std::uint64_t expected_token,
        const std::function<void()> &publication) const override {
        ++capability_calls;
        if (lose_before_capability || account != account_ ||
            expected_token != token || !publication)
            return false;
        publication();
        return true;
    }

    mutable std::size_t calls = 0;
    mutable std::size_t capability_calls = 0;
    std::size_t lose_on_call = 0;
    bool lose_before_capability = false;
    std::uint64_t token = 7;
    mt5bridge::AccountKey account_ = account();
};

struct Fixture {
    mt5bridge_test_support::MemoryJournalStore journal;
    MemorySettlementStore settlement;
    MemoryBrokerStore broker_store;
    MemoryBasisStore basis;
    MemoryEnvelopeStore envelope_store;
    mt5bridge::BrokerReversalRecord broker_record = broker();

    mt5bridge::ManagedBrokerVolumeMapping add_source(
        std::uint64_t trade_id, std::uint64_t operation_id,
        std::uint64_t operation_revision = 11) {
        const mt5bridge::OperationKey operation_key{account(), trade_id,
                                                     operation_id};
        journal.records[operation_key] =
            operation(operation_key, operation_revision, 701);

        mt5bridge::ManagedDealSettlement fact;
        fact.key = {account(), operation_key, 701};
        fact.source_operation_revision = 8;
        fact.managed_logical_units = 2;
        fact.provenance = settlement_provenance();
        require(fact.valid(), "settlement fixture is invalid");
        settlement.records[fact.key] = fact;

        mt5bridge::ManagedOwnershipBasis basis_record;
        basis_record.account = account();
        basis_record.basis_key = {broker_record.key(), operation_key};
        basis_record.broker_provenance = broker_record.provenance;
        basis_record.operation_revision = 11;
        require(basis_record.valid(), "basis fixture is invalid");
        basis.records[basis_record.key()] = basis_record;

        const auto mapping = mt5bridge::derive_managed_broker_volume_mapping(
            fact, broker_record);
        require(mapping.has_value(), "mapping fixture is invalid");
        return *mapping;
    }

    void finalize_sources() {
        broker_store.records[broker_record.key()] = broker_record;
        envelope_store.records[broker_record.key()] = envelope(broker_record);
    }
};

mt5bridge::ManagedAllocationEntry entry_for(
    const mt5bridge::ManagedBrokerVolumeMapping &mapping,
    std::uint64_t basis_revision = 11) {
    mt5bridge::ManagedAllocationEntry result;
    result.account = account();
    result.allocation_key = {mapping.broker_key(), mapping.managed_key(),
                             mt5bridge::ManagedAllocationLeg::close};
    result.settlement_provenance = mapping.managed_provenance();
    result.broker_provenance = mapping.broker_provenance();
    result.settlement_source_operation_revision = 8;
    result.basis_operation_revision = basis_revision;
    result.managed_logical_units = mapping.managed_logical_units();
    result.broker_volume = mapping.broker_volume();
    require(result.valid(), "allocation entry fixture is invalid");
    return result;
}

mt5bridge::ManagedAllocationCommitStatus commit(
    mt5bridge::WindowsFileManagedAllocationStore &store,
    const mt5bridge::ManagedAllocationEntry &entry,
    const mt5bridge::ManagedBrokerVolumeMapping &mapping, Fixture &fixture,
    const mt5bridge::SingleWriterLease &lease) {
    return store.commit(entry, mapping, fixture.journal, fixture.settlement,
                        fixture.broker_store, fixture.basis,
                        fixture.envelope_store, lease);
}

void run_regressions() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_allocation_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);

    Fixture fixture;
    fixture.finalize_sources();
    const auto mapping_a = fixture.add_source(71, 4);
    const auto mapping_b = fixture.add_source(72, 5);
    const auto mapping_c = fixture.add_source(73, 6);
    const auto mapping_d = fixture.add_source(74, 7);

    mt5bridge::WindowsFileManagedAllocationStore store(root);
    require(store.ready(), "managed allocation store did not open");
    TestLease lease;

    const auto entry_a = entry_for(mapping_a);
    require(commit(store, entry_a, mapping_a, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::committed,
            "first allocation commit failed");
    {
        std::ofstream interrupted(root / "alloc-interrupted.bin.tmp",
                                  std::ios::binary);
        interrupted << "unpublished temporary bytes";
    }

    fixture.journal.records[entry_a.allocation_key.managed_key.operation_key] =
        operation(entry_a.allocation_key.managed_key.operation_key, 12, 701);
    require(commit(store, entry_a, mapping_a, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::already_committed,
            "historical exact replay was not idempotent");

    const auto entry_b = entry_for(mapping_b);
    require(commit(store, entry_b, mapping_b, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::committed,
            "second allocation within close capacity failed");

    auto conflicting = entry_a;
    conflicting.basis_operation_revision = 12;
    require(commit(store, conflicting, mapping_a, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::conflict,
            "same allocation key with different payload was accepted");

    lease.calls = 0;
    lease.capability_calls = 0;
    lease.lose_on_call = 0;
    lease.lose_before_capability = true;
    const auto entry_d = entry_for(mapping_d);
    require(commit(store, entry_d, mapping_d, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::lease_not_held,
            "stale writer published after fencing handoff");
    require(lease.capability_calls == 1,
            "allocation publication did not acquire fencing capability");
    require(store.load(entry_d.allocation_key).status ==
                mt5bridge::ManagedAllocationLoadStatus::not_found,
            "fenced writer left an allocation file behind");

    lease.calls = 0;
    lease.capability_calls = 0;
    lease.lose_on_call = 0;
    lease.lose_before_capability = false;
    lease.token = 9;
    require(commit(store, entry_d, mapping_d, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::committed,
            "new fenced writer could not commit");
    require(commit(store, entry_d, mapping_d, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::already_committed,
            "new fenced writer replay was not idempotent");

    const auto entry_c = entry_for(mapping_c);
    require(commit(store, entry_c, mapping_c, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::capacity_exhausted,
            "close-leg capacity overflow was accepted");

    const auto pending_mapping = fixture.add_source(75, 8);
    const auto pending_key = pending_mapping.managed_key();
    fixture.settlement.records.erase(pending_key);
    const auto pending_entry = entry_for(pending_mapping);
    require(commit(store, pending_entry, pending_mapping, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::missing_source,
            "pending-only settlement fact was admitted");

    auto reverse_open = entry_a;
    reverse_open.allocation_key.leg =
        static_cast<mt5bridge::ManagedAllocationLeg>(1);
    require(!reverse_open.valid(), "unsupported reverse-open leg was valid");
    require(commit(store, reverse_open, mapping_a, fixture, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_record,
            "unsupported reverse-open allocation was admitted");

    mt5bridge::WindowsFileManagedAllocationStore restarted(root);
    require(restarted.ready(), "restarted allocation store did not open");
    const auto scan = restarted.scan(fixture.settlement, fixture.broker_store,
                                     fixture.basis, fixture.envelope_store);
    require(scan.complete() && scan.records.size() == 3,
            "historical allocation scan did not recover committed entries");

    auto inconsistent_envelope = fixture.envelope_store.records.at(
        fixture.broker_record.key());
    inconsistent_envelope.broker_close_leg.units += 100;
    inconsistent_envelope.unallocated_close_leg =
        inconsistent_envelope.broker_close_leg;
    require(inconsistent_envelope.valid(),
            "inconsistent envelope fixture is not structurally valid");
    fixture.envelope_store.records[fixture.broker_record.key()] =
        inconsistent_envelope;
    require(restarted.scan(fixture.settlement, fixture.broker_store,
                           fixture.basis, fixture.envelope_store)
                .status == mt5bridge::ManagedAllocationScanStatus::invalid_record,
            "historical scan accepted envelope capacity detached from broker proof");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    try {
        run_regressions();
    } catch (const std::exception &error) {
        return (std::cerr << error.what() << '\n', 1);
    }
    return 0;
}

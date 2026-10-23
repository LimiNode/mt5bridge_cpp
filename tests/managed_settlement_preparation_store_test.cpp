/// \file managed_settlement_preparation_store_test.cpp
/// \brief Exercises durable staged settlement preparation and restart finalization.

#include "managed_deal_settlement_producer.hpp"

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>
#include <mt5bridge/dispatch/file_managed_deal_settlement_store.hpp>
#include <mt5bridge/dispatch/file_managed_ownership_basis_store.hpp>
#include <mt5bridge/dispatch/file_managed_settlement_preparation_store.hpp>
#include <mt5bridge/dispatch/managed_staged_allocation_admission.hpp>

#include "support/memory_journal_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::BrokerReversalProvenance broker_provenance() {
    return {11, 8, 12, 8, 12, 10, 202};
}

Mt5DealSnapshot deal_snapshot() {
    Mt5DealSnapshot value{};
    value.ticket = 701;
    value.order_ticket = 900;
    value.position_id = 901;
    value.entry = 1;
    value.volume = 2.0;
    value.time_msc = 1500;
    value.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                         MT5BRIDGE_DEAL_KNOWN_ENTRY |
                         MT5BRIDGE_DEAL_KNOWN_VOLUME |
                         MT5BRIDGE_DEAL_KNOWN_TIME;
    return value;
}

mt5bridge::BrokerReversalRecord broker_record() {
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
                         mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt}
                   : mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::found, found->second};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        mt5bridge::BrokerReversalScanResult result;
        result.status = mt5bridge::BrokerReversalScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

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
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerAllocationEnvelope> records;
};

class TestLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &requested) const override {
        return requested == owned_account ? std::optional<std::uint64_t>(token)
                                          : std::nullopt;
    }

    bool with_fencing_capability(
        const mt5bridge::AccountKey &requested, std::uint64_t expected_token,
        const std::function<void()> &publication) const override {
        if (!(requested == owned_account) || expected_token != token || !publication)
            return false;
        publication();
        return true;
    }

    mt5bridge::AccountKey owned_account = account();
    std::uint64_t token = 7;
};

class MemorySettlementStore final : public mt5bridge::DurableManagedDealSettlementStore {
public:
    mt5bridge::ManagedDealSettlementCommitStatus commit(
        const mt5bridge::ManagedDealSettlementCommit &,
        const mt5bridge::DurableJournalStore &) override {
        return mt5bridge::ManagedDealSettlementCommitStatus::io_error;
    }

    mt5bridge::ManagedDealSettlementLoadResult load(
        const mt5bridge::ManagedDealSettlementKey &key) const override {
        if (!fact || !(fact->key == key))
            return {mt5bridge::ManagedDealSettlementLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedDealSettlementLoadStatus::found, fact};
    }

    mt5bridge::ManagedSettlementFrontierLoadResult load_frontier(
        const mt5bridge::OperationKey &key, std::uint64_t revision) const override {
        if (!frontier || !(frontier->operation_key == key) ||
            frontier->operation_revision != revision)
            return {mt5bridge::ManagedSettlementFrontierLoadStatus::not_found,
                    std::nullopt};
        return {mt5bridge::ManagedSettlementFrontierLoadStatus::found, frontier};
    }

    std::optional<mt5bridge::ManagedDealSettlement> fact;
    std::optional<mt5bridge::ManagedSettlementFrontier> frontier;
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
        if (!basis || !(basis->key() == key))
            return {mt5bridge::ManagedOwnershipBasisLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedOwnershipBasisLoadStatus::found, basis};
    }

    mt5bridge::ManagedOwnershipBasisScanResult scan() const override {
        mt5bridge::ManagedOwnershipBasisScanResult result;
        result.status = mt5bridge::ManagedOwnershipBasisScanStatus::complete;
        if (basis)
            result.records.push_back(*basis);
        return result;
    }

    std::optional<mt5bridge::ManagedOwnershipBasis> basis;
};

class MemoryAllocationStore final : public mt5bridge::DurableManagedAllocationStore {
public:
    mt5bridge::ManagedAllocationCommitStatus commit(
        const mt5bridge::ManagedAllocationEntry &,
        const mt5bridge::ManagedBrokerVolumeMapping &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::SingleWriterLease &) override {
        return mt5bridge::ManagedAllocationCommitStatus::io_error;
    }

    mt5bridge::ManagedAllocationCommitStatus commit_staged(
        const mt5bridge::ManagedAllocationEntry &,
        const mt5bridge::ManagedBrokerVolumeMapping &,
        const mt5bridge::ManagedSettlementPreparation &,
        const mt5bridge::DurableManagedSettlementPreparationStore &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::SingleWriterLease &) override {
        return mt5bridge::ManagedAllocationCommitStatus::io_error;
    }

    mt5bridge::ManagedAllocationLoadResult load(
        const mt5bridge::ManagedAllocationKey &key) const override {
        if (!allocation || !(allocation->allocation_key == key))
            return {mt5bridge::ManagedAllocationLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedAllocationLoadStatus::found, allocation};
    }

    mt5bridge::ManagedAllocationScanResult scan(
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &) const override {
        return scan_result();
    }

    mt5bridge::ManagedAllocationScanResult scan_recovery(
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::DurableManagedSettlementPreparationStore &) const override {
        return scan_result();
    }

    std::optional<mt5bridge::ManagedAllocationEntry> allocation;

private:
    mt5bridge::ManagedAllocationScanResult scan_result() const {
        mt5bridge::ManagedAllocationScanResult result;
        result.status = mt5bridge::ManagedAllocationScanStatus::complete;
        if (allocation)
            result.records.push_back(*allocation);
        return result;
    }
};

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

mt5bridge::OperationRecord operation(
    const mt5bridge::ReconciliationBaseline &baseline, std::uint64_t revision,
    std::uint64_t settled_volume) {
    const auto key = mt5bridge::OperationKey{account(), 71, 4};
    mt5bridge::ReconciliationDescriptor descriptor{
        key.account,
        baseline,
        {mt5bridge::require_history_deal(701,
                                         mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::partially_filled,
        key.trade_id,
        key.operation_id,
        5};

    mt5bridge::OperationRecord record;
    record.key = key;
    record.operation_kind = mt5bridge::OperationKind::close;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = settled_volume == 0
                                 ? mt5bridge::OperationState::reconciling
                                 : mt5bridge::OperationState::partially_filled;
    record.journal_state = mt5bridge::JournalState::reconciling;
    record.revision = revision;
    record.fencing_token = 3;
    record.reconciliation_descriptor = std::move(descriptor);
    record.settled_volume = settled_volume;
    require(record.valid(), "operation fixture is invalid");
    return record;
}

struct DurableSources {
    MemoryBrokerStore broker;
    MemoryEnvelopeStore envelope;
    mt5bridge::BrokerReversalRecord reversal = broker_record();

    DurableSources() {
        broker.records[reversal.key()] = reversal;
        envelope.records[reversal.key()] = ::envelope(reversal);
    }
};

struct ProofData {
    mt5bridge::ReconciliationBaseline baseline;
    mt5bridge::OperationRecord base_operation;
    mt5bridge::OperationRecord target_operation;
    mt5bridge::ManagedDealSettlementCommit proof;
};

struct PreparedFixture {
    mt5bridge_test_support::MemoryJournalStore journal;
    DurableSources sources;
    ProofData data;
    mt5bridge::ManagedStagedOwnershipAssociation staged;
    mt5bridge::ManagedBrokerVolumeMapping mapping;
    mt5bridge::ManagedAllocationEntry allocation;

    PreparedFixture()
        : sources(),
          data(make_proof_data()),
          staged{account(),
                 {sources.reversal.key(), data.target_operation.key},
                 sources.reversal.provenance,
                 8,
                 9,
                 0xfeed},
          mapping(*mt5bridge::derive_managed_broker_volume_mapping(
              data.proof.fact(), sources.reversal)),
          allocation(make_allocation(data.proof.fact(), data.proof.frontier(), mapping,
                                     sources.reversal)) {
        journal.records[data.base_operation.key] = data.base_operation;
        require(staged.valid(), "staged ownership fixture is invalid");
        require(mapping.valid(), "managed mapping fixture is invalid");
        require(allocation.valid(), "staged allocation fixture is invalid");
    }

private:
    static ProofData make_proof_data() {
        mt5bridge::ObservationGraph graph(account());
        mt5bridge::ObservationBatch batch;
        batch.account = account();
        batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        require(graph.apply(batch).accepted(), "baseline observation failed");
        const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);
        const auto base_operation = operation(baseline, 8, 0);
        const auto target_operation = operation(baseline, 9, 2);
        mt5bridge::ObservationBatch deal_batch;
        deal_batch.account = account();
        deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        deal_batch.history_deals = {deal_snapshot()};
        require(graph.apply(deal_batch).accepted(), "proof deal observation failed");
        const auto proof = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
            target_operation, baseline, graph, 701, 2);
        require(proof.has_value(), "staged settlement proof was not derived");
        return {baseline, base_operation, target_operation, *proof};
    }

public:
    static mt5bridge::ManagedAllocationEntry make_allocation(
        const mt5bridge::ManagedDealSettlement &fact,
        const mt5bridge::ManagedSettlementFrontier &frontier,
        const mt5bridge::ManagedBrokerVolumeMapping &mapping,
        const mt5bridge::BrokerReversalRecord &reversal) {
        mt5bridge::ManagedAllocationEntry result;
        result.account = account();
        result.allocation_key = {reversal.key(), fact.key,
                                 mt5bridge::ManagedAllocationLeg::close};
        result.settlement_provenance = fact.provenance;
        result.broker_provenance = reversal.provenance;
        result.settlement_source_operation_revision = fact.source_operation_revision;
        result.settlement_frontier_revision = frontier.operation_revision;
        result.settlement_frontier_digest =
            mt5bridge::managed_settlement_frontier_digest(frontier);
        result.basis_operation_revision = frontier.operation_revision;
        result.managed_logical_units = fact.managed_logical_units;
        result.broker_volume = mapping.broker_volume();
        return result;
    }
};

void run_restart_regression() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_settlement_preparation_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto settlement_root = root / "settlement";
    const auto allocation_root = root / "allocation";
    const auto basis_root = root / "basis";
    const auto preparation_root = root / "preparation";

    {
        PreparedFixture late_partial_fixture;
        auto late_partial_base = late_partial_fixture.data.base_operation;
        late_partial_base.operation_state = mt5bridge::OperationState::partially_filled;
        late_partial_base.settled_volume = 1;
        require(late_partial_base.valid(), "late-partial base fixture is malformed");
        late_partial_fixture.journal.records[late_partial_base.key] = late_partial_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore late_partial_store(
            root / "late-partial");
        require(late_partial_store.prepare(
                    late_partial_fixture.data.proof,
                    late_partial_fixture.data.target_operation, 8,
                    late_partial_fixture.staged, late_partial_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "valid incremental partial settlement was rejected");
    }

    {
        PreparedFixture forged_fixture;
        auto forged_target = forged_fixture.data.target_operation;
        forged_target.request_payload.push_back(0x7f);
        mt5bridge::WindowsFileManagedSettlementPreparationStore forged_store(
            root / "forged-immutable-target");
        require(forged_store.prepare(forged_fixture.data.proof, forged_target, 8,
                                     forged_fixture.staged, forged_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_operation_record,
                "preparation accepted a target with forged immutable payload");
    }

    {
        PreparedFixture invalid_transition_fixture;
        auto invalid_base = invalid_transition_fixture.data.base_operation;
        invalid_base.operation_state = mt5bridge::OperationState::accepted;
        require(invalid_base.valid(), "invalid-transition base fixture is malformed");
        invalid_transition_fixture.journal.records[invalid_base.key] = invalid_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore invalid_transition_store(
            root / "invalid-transition");
        require(invalid_transition_store.prepare(
                    invalid_transition_fixture.data.proof,
                    invalid_transition_fixture.data.target_operation, 8,
                    invalid_transition_fixture.staged, invalid_transition_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_operation_record,
                "preparation accepted an invalid lifecycle transition");
    }

    {
        PreparedFixture zero_requested_fixture;
        auto zero_requested_base = zero_requested_fixture.data.base_operation;
        auto zero_requested_target = zero_requested_fixture.data.target_operation;
        require(zero_requested_base.reconciliation_descriptor.has_value(),
                "zero-volume base fixture has no reconciliation descriptor");
        require(zero_requested_target.reconciliation_descriptor.has_value(),
                "zero-volume target fixture has no reconciliation descriptor");
        zero_requested_base.reconciliation_descriptor->requested_volume = 0;
        zero_requested_target.reconciliation_descriptor->requested_volume = 0;
        require(zero_requested_base.valid(), "zero-volume base fixture is malformed");
        require(zero_requested_target.valid(), "zero-volume target fixture is malformed");
        zero_requested_fixture.journal.records[zero_requested_base.key] = zero_requested_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore zero_requested_store(
            root / "zero-requested-volume");
        require(zero_requested_store.prepare(
                    zero_requested_fixture.data.proof, zero_requested_target, 8,
                    zero_requested_fixture.staged, zero_requested_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_record,
                "preparation admitted a zero requested volume");
    }

    mt5bridge::OperationRecord durable_base_operation;
    {
        PreparedFixture fixture;
        mt5bridge::WindowsFileManagedSettlementPreparationStore preparation_store(
            preparation_root);
        require(preparation_store.ready(), "preparation store did not open");
        require(preparation_store.prepare(
                    fixture.data.proof, fixture.data.target_operation, 8, fixture.staged,
                    fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "durable preparation did not publish");

        mt5bridge::WindowsFileManagedDealSettlementStore settlement_store(settlement_root);
        require(settlement_store.ready(), "settlement store did not open");
        require(settlement_store.load(fixture.data.proof.fact().key).status ==
                    mt5bridge::ManagedDealSettlementLoadStatus::not_found,
                "prepared fact was admitted before finalization");
        const auto replay_status = preparation_store.prepare(
                    fixture.data.proof, fixture.data.target_operation, 8, fixture.staged,
                    fixture.journal);
        if (replay_status !=
            mt5bridge::ManagedSettlementPreparationStatus::already_prepared) {
            const auto replay_scan = preparation_store.scan();
            require(replay_scan.complete() && replay_scan.records.size() == 1,
                    "preparation replay manifest could not be decoded");
            require(replay_scan.records.front() ==
                        mt5bridge::ManagedSettlementPreparation{
                            fixture.data.proof.fact(), fixture.data.proof.frontier(),
                            fixture.data.target_operation, 8, fixture.staged},
                    "preparation replay payload changed");
            throw std::runtime_error("preparation replay returned unexpected status");
        }
        durable_base_operation = fixture.data.base_operation;
    }

    mt5bridge_test_support::MemoryJournalStore restarted_journal;
    restarted_journal.records[durable_base_operation.key] = durable_base_operation;
    DurableSources restarted_sources;
    mt5bridge::WindowsFileManagedSettlementPreparationStore restarted_preparation(
        preparation_root);
    mt5bridge::WindowsFileManagedDealSettlementStore restarted_settlement(settlement_root);
    mt5bridge::WindowsFileManagedAllocationStore restarted_allocation(allocation_root);
    mt5bridge::WindowsFileManagedOwnershipBasisStore restarted_basis(basis_root);
    require(restarted_preparation.ready() && restarted_settlement.ready() &&
                restarted_allocation.ready() && restarted_basis.ready(),
            "restarted Stage B stores did not open");

    const auto pending = restarted_preparation.scan();
    require(pending.complete() && pending.records.size() == 1,
            "restart did not recover the exact preparation manifest");
    const auto &recovered_preparation = pending.records.front();
    require(recovered_preparation.target_operation.key == durable_base_operation.key &&
                recovered_preparation.target_operation.revision == 9 &&
                recovered_preparation.base_operation_revision == 8 &&
                recovered_preparation.fact.key.deal_ticket == 701,
            "restart manifest changed the staged recipe");

    const auto recovered_mapping = mt5bridge::derive_managed_broker_volume_mapping(
        recovered_preparation.fact, restarted_sources.reversal);
    require(recovered_mapping.has_value(),
            "restart could not reconstruct the managed broker mapping");
    const auto recovered_allocation = PreparedFixture::make_allocation(
        recovered_preparation.fact, recovered_preparation.frontier, *recovered_mapping,
        restarted_sources.reversal);

    const auto staged_admission = mt5bridge::admit_staged_managed_allocation(
        recovered_allocation, *recovered_mapping, recovered_preparation, restarted_preparation,
        restarted_journal, restarted_settlement, restarted_sources.broker, restarted_basis,
        restarted_sources.envelope, restarted_allocation);
    require(staged_admission.status ==
                mt5bridge::ManagedStagedAllocationAdmissionStatus::invalid_broker,
            "unsupported INOUT source was admitted without an authoritative close proof");
    auto mismatched_frontier = recovered_allocation;
    mismatched_frontier.settlement_frontier_digest =
        *mismatched_frontier.settlement_frontier_digest + 1;
    require(mt5bridge::admit_staged_managed_allocation(
                mismatched_frontier, *recovered_mapping, recovered_preparation,
                restarted_preparation, restarted_journal, restarted_settlement,
                restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, restarted_allocation)
                .status == mt5bridge::ManagedStagedAllocationAdmissionStatus::invalid_mapping,
            "staged admission accepted a mismatched frontier digest");

    TestLease no_lease;
    no_lease.owned_account = {"Other-Account", 7};
    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, no_lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::lease_not_held,
            "recovery mutated without an authoritative account lease");

    TestLease lease;
    auto forged_preparation = recovered_preparation;
    forged_preparation.target_operation.request_payload.push_back(0x7f);
    require(forged_preparation.valid(), "forged preparation fixture is malformed");
    require(restarted_preparation.finalize(
                forged_preparation, restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker, restarted_allocation,
                lease) == mt5bridge::ManagedSettlementFinalizeStatus::conflict,
            "finalize accepted a preparation that differs from the durable manifest");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "manifest mismatch changed the journal before rejection");

    mt5bridge::WindowsFileManagedSettlementPreparationStore missing_preparation(
        root / "missing-preparation");
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                missing_preparation, restarted_journal, restarted_settlement,
                restarted_sources.broker, restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "staged allocation published without a durable preparation manifest");
    require(restarted_allocation.load(recovered_allocation.allocation_key).status ==
                mt5bridge::ManagedAllocationLoadStatus::not_found,
            "manifest-less staged allocation consumed capacity");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "unsupported INOUT preparation passed finalization without an authoritative proof");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "pending allocation recovery advanced the journal early");
    require(mt5bridge::validate_managed_ownership_basis(
                recovered_preparation.staged_ownership.settled_basis(), restarted_journal,
                restarted_sources.envelope) ==
                mt5bridge::ManagedOwnershipBasisProofStatus::invalid_operation_record,
            "staged ownership passed ordinary validation before journal CAS");

    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "unsupported INOUT allocation was committed without an authoritative proof");
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "unsupported INOUT allocation replay changed its fail-closed result");
    require(restarted_settlement.load(recovered_preparation.fact.key).status ==
                mt5bridge::ManagedDealSettlementLoadStatus::not_found,
            "allocation publication leaked an unfinalized settlement fact");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "recovery advanced an unsupported INOUT preparation without an allocation proof");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "fail-closed recovery advanced the journal");

    {
        PreparedFixture historical_fixture;
        const auto historical_root = root / "historical-replay";
        mt5bridge::WindowsFileManagedSettlementPreparationStore historical_preparation_store(
            historical_root);
        require(historical_preparation_store.prepare(
                    historical_fixture.data.proof, historical_fixture.data.target_operation, 8,
                    historical_fixture.staged, historical_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "historical replay preparation did not publish");

        const mt5bridge::ManagedSettlementPreparation historical_preparation{
            historical_fixture.data.proof.fact(), historical_fixture.data.proof.frontier(),
            historical_fixture.data.target_operation, 8, historical_fixture.staged};
        MemorySettlementStore historical_settlement;
        historical_settlement.fact = historical_preparation.fact;
        historical_settlement.frontier = historical_preparation.frontier;
        MemoryBasisStore historical_basis;
        historical_basis.basis = historical_preparation.staged_ownership.settled_basis();
        MemoryAllocationStore historical_allocation;
        historical_allocation.allocation = historical_fixture.allocation;
        mt5bridge_test_support::MemoryJournalStore historical_journal;
        historical_journal.records[historical_preparation.target_operation.key] =
            operation(historical_fixture.data.baseline, 10, 3);
        require(historical_journal.records.at(historical_preparation.target_operation.key)
                    .valid(),
                "historical replay journal fixture is malformed");

        require(historical_preparation_store.finalize(
                    historical_preparation, historical_journal, historical_settlement,
                    historical_basis, historical_fixture.sources.envelope,
                    historical_fixture.sources.broker, historical_allocation, lease) ==
                    mt5bridge::ManagedSettlementFinalizeStatus::already_finalized,
                "completed historical preparation was not skipped after journal advancement");
        require(historical_journal.records.at(historical_preparation.target_operation.key)
                        .revision == 10,
                "historical replay changed the newer journal revision");
    }

    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs durable staged preparation and restart recovery checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        run_restart_regression();
        std::cout << "managed settlement preparation checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "managed settlement preparation checks failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

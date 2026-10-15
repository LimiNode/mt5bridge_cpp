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
        recovered_allocation, *recovered_mapping, recovered_preparation, restarted_journal,
        restarted_settlement, restarted_sources.broker, restarted_basis,
        restarted_sources.envelope, restarted_allocation);
    require(staged_admission.admitted(),
            "staged admission did not accept the exact prepared frontier");
    auto mismatched_frontier = recovered_allocation;
    mismatched_frontier.settlement_frontier_digest =
        *mismatched_frontier.settlement_frontier_digest + 1;
    require(mt5bridge::admit_staged_managed_allocation(
                mismatched_frontier, *recovered_mapping, recovered_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, restarted_allocation)
                .status == mt5bridge::ManagedStagedAllocationAdmissionStatus::invalid_mapping,
            "staged admission accepted a mismatched frontier digest");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation) ==
                mt5bridge::ManagedSettlementFinalizeStatus::pending_allocation,
            "pending preparation was finalized without allocation gate");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "pending allocation recovery advanced the journal early");
    require(mt5bridge::validate_managed_ownership_basis(
                recovered_preparation.staged_ownership.settled_basis(), restarted_journal,
                restarted_sources.envelope) ==
                mt5bridge::ManagedOwnershipBasisProofStatus::invalid_operation_record,
            "staged ownership passed ordinary validation before journal CAS");

    TestLease lease;
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::committed,
            "staged allocation commit failed");
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::already_committed,
            "staged allocation replay was not idempotent before settlement finalization");
    require(restarted_settlement.load(recovered_preparation.fact.key).status ==
                mt5bridge::ManagedDealSettlementLoadStatus::not_found,
            "allocation publication leaked an unfinalized settlement fact");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation) ==
                mt5bridge::ManagedSettlementFinalizeStatus::finalized,
            "restart recovery did not finalize the allocation-gated recipe");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 9,
            "finalization did not CAS the target operation");
    require(restarted_settlement.load(recovered_preparation.fact.key).found(),
            "finalization did not publish the managed settlement fact");
    require(restarted_basis.load(recovered_preparation.staged_ownership.settled_basis().key()).found(),
            "finalization did not publish staged ownership basis");
    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation) ==
                mt5bridge::ManagedSettlementFinalizeStatus::already_finalized,
            "finalized preparation replay was not idempotent");

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

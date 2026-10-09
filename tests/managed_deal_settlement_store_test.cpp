/// \file managed_deal_settlement_store_test.cpp
/// \brief Exercises immutable per-deal facts and cumulative settlement frontiers.

#include "managed_deal_settlement_producer.hpp"

#include <mt5bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

Mt5DealSnapshot deal(std::uint64_t ticket, double volume) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = 900;
    value.position_id = 901;
    value.entry = 1;
    value.volume = volume;
    value.time_msc = 1500;
    value.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                         MT5BRIDGE_DEAL_KNOWN_ENTRY |
                         MT5BRIDGE_DEAL_KNOWN_VOLUME |
                         MT5BRIDGE_DEAL_KNOWN_TIME;
    return value;
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
        settled_volume == 5 ? mt5bridge::OperationState::filled
                            : mt5bridge::OperationState::partially_filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = 5;

    mt5bridge::OperationRecord record;
    record.key = key;
    record.operation_kind = mt5bridge::OperationKind::close;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = descriptor.settled_state;
    record.journal_state = mt5bridge::JournalState::reconciling;
    record.revision = revision;
    record.fencing_token = 3;
    record.reconciliation_descriptor = std::move(descriptor);
    record.settled_volume = settled_volume;
    require(record.valid(), "settled close operation fixture is invalid");
    return record;
}

class MemoryJournalStore final : public mt5bridge::DurableJournalStore {
public:
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t>) override {
        records[record.key] = record;
        return mt5bridge::StoreCommitStatus::committed;
    }

    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::not_found,
                                                std::nullopt}
                   : mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::found,
                                                found->second};
    }

    mt5bridge::StoreScanResult scan() const override {
        mt5bridge::StoreScanResult result;
        result.status = mt5bridge::StoreScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> records;
};

void check_incremental_frontier() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_deal_settlement_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);

    MemoryJournalStore journal;
    mt5bridge::ObservationGraph first_graph(account());
    mt5bridge::ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    baseline_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    require(first_graph.apply(baseline_batch).accepted(), "first baseline failed");
    const auto first_baseline = mt5bridge::capture_reconciliation_baseline(first_graph);
    const auto first_operation = operation(first_baseline, 8, 2);
    journal.records[first_operation.key] = first_operation;
    mt5bridge::ObservationBatch first_deal_batch;
    first_deal_batch.account = account();
    first_deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    first_deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    first_deal_batch.history_deals = {deal(701, 2.0)};
    require(first_graph.apply(first_deal_batch).accepted(), "first deal observation failed");
    const auto first_proof = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
        first_operation, first_baseline, first_graph, 701, 2);
    require(first_proof.has_value(), "first immutable deal proof was not derived");
    const auto &first_fact = first_proof->fact();
    const auto &first_frontier = first_proof->frontier();
    require(first_frontier.valid(), "first settlement frontier was not complete");

    mt5bridge::WindowsFileManagedDealSettlementStore store(root);
    require(store.ready(), "managed deal settlement store did not open");
    require(store.commit(*first_proof, journal) ==
                mt5bridge::ManagedDealSettlementCommitStatus::committed,
            "first deal/frontier commit failed");
    require(store.commit(*first_proof, journal) ==
                mt5bridge::ManagedDealSettlementCommitStatus::already_committed,
            "first deal/frontier replay was not idempotent");

    const auto second_operation = operation(first_baseline, 11, 5);
    journal.records[second_operation.key] = second_operation;
    const auto second_baseline = first_baseline;
    mt5bridge::ObservationBatch second_deal_batch;
    second_deal_batch.account = account();
    second_deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    second_deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    second_deal_batch.history_deals = {deal(701, 2.0), deal(702, 3.0)};
    require(first_graph.apply(second_deal_batch).accepted(), "second deal observation failed");
    const auto second_proof = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
        second_operation, second_baseline, first_graph, 702, 3, {first_fact});
    require(second_proof.has_value(), "second immutable deal proof was not derived");
    const auto &second_fact = second_proof->fact();
    const auto &second_frontier = second_proof->frontier();
    require(second_frontier.valid(), "incremental settlement frontier was not complete");
    require(store.commit(*second_proof, journal) ==
                mt5bridge::ManagedDealSettlementCommitStatus::committed,
            "incremental deal/frontier commit failed");

    const auto loaded_first = store.load(first_fact.key);
    require(loaded_first.found() && *loaded_first.record == first_fact,
            "the earlier per-deal fact was rewritten");
    const auto scan = store.scan();
    require(scan.complete() && scan.facts.size() == 2 && scan.frontiers.size() == 2,
            "restart scan did not recover facts and frontiers");

    const auto bad_reattribution =
        mt5bridge::dispatch::ManagedDealSettlementProducer::create(
            second_operation, second_baseline, first_graph, 701, 5, {first_fact});
    require(!bad_reattribution.has_value(),
            "cumulative volume was accepted as a new contribution for an old deal");

    std::filesystem::path frontier_to_remove;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        const auto filename = entry.path().filename().wstring();
        if (entry.path().extension() == L".bin" && filename.rfind(L"frontier-", 0) == 0 &&
            (frontier_to_remove.empty() ||
             filename > frontier_to_remove.filename().wstring())) {
            frontier_to_remove = entry.path();
        }
    }
    require(!frontier_to_remove.empty(), "frontier fixture was not written");
    std::filesystem::remove(frontier_to_remove);
    const auto orphan_scan = store.scan();
    require(orphan_scan.complete() && orphan_scan.facts.size() == 1 &&
                orphan_scan.pending_facts.size() == 1 && orphan_scan.frontiers.size() == 1,
            "restart scan did not preserve an unpublished fact as pending");

    require(store.recover_pending(journal) ==
                mt5bridge::ManagedDealSettlementCommitStatus::committed,
            "pending publication replay was not recoverable");
    const auto recovered_scan = store.scan();
    require(recovered_scan.complete() && recovered_scan.facts.size() == 2 &&
                recovered_scan.pending_facts.empty() && recovered_scan.frontiers.size() == 2,
            "pending fact/frontier replay did not restore the committed set");

    std::filesystem::path first_fact_to_remove;
    const std::wstring first_fact_suffix = L"-00000000000002bd.bin";
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        const auto filename = entry.path().filename().wstring();
        if (entry.path().extension() == L".bin" && filename.rfind(L"deal-", 0) == 0 &&
            filename.size() >= first_fact_suffix.size() &&
            filename.compare(filename.size() - first_fact_suffix.size(),
                             first_fact_suffix.size(), first_fact_suffix) == 0) {
            first_fact_to_remove = entry.path();
            break;
        }
    }
    require(!first_fact_to_remove.empty(), "first fact fixture was not written");
    std::filesystem::remove(first_fact_to_remove);
    require(store.scan().status == mt5bridge::ManagedDealSettlementScanStatus::invalid_record,
            "restart scan accepted a frontier with a missing fact");
    std::filesystem::remove_all(root);
}

void check_restart_recovery() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_deal_restart_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);

    mt5bridge::OperationRecord durable_operation;
    {
        MemoryJournalStore journal;
        mt5bridge::ObservationGraph graph(account());
        mt5bridge::ObservationBatch baseline_batch;
        baseline_batch.account = account();
        baseline_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        baseline_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        require(graph.apply(baseline_batch).accepted(), "restart baseline failed");
        const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);
        const auto source_operation = operation(baseline, 8, 2);
        journal.records[source_operation.key] = source_operation;
        durable_operation = source_operation;

        mt5bridge::ObservationBatch deal_batch;
        deal_batch.account = account();
        deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        deal_batch.history_deals = {deal(701, 2.0)};
        require(graph.apply(deal_batch).accepted(), "restart deal observation failed");
        const auto proof = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
            source_operation, baseline, graph, 701, 2);
        require(proof.has_value(), "restart proof was not derived");

        mt5bridge::WindowsFileManagedDealSettlementStore store(root);
        require(store.ready(), "restart store did not open");
        require(store.commit(*proof, journal) ==
                    mt5bridge::ManagedDealSettlementCommitStatus::committed,
                "restart fixture commit failed");
    }

    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        const auto filename = entry.path().filename().wstring();
        if (entry.path().extension() == L".bin" && filename.rfind(L"frontier-", 0) == 0) {
            std::filesystem::remove(entry.path());
            break;
        }
    }

    MemoryJournalStore restarted_journal;
    restarted_journal.records[durable_operation.key] = durable_operation;
    mt5bridge::WindowsFileManagedDealSettlementStore restarted_store(root);
    require(restarted_store.ready(), "restarted store did not open");
    const auto pending = restarted_store.scan();
    require(pending.complete() && pending.facts.empty() && pending.frontiers.empty() &&
                pending.pending_facts.size() == 1,
            "restart scan did not recover the durable pending publication");
    require(restarted_store.recover_pending(restarted_journal) ==
                mt5bridge::ManagedDealSettlementCommitStatus::committed,
            "restart recovery did not publish the durable frontier");
    const auto recovered = restarted_store.scan();
    require(recovered.complete() && recovered.facts.size() == 1 &&
                recovered.pending_facts.empty() && recovered.frontiers.size() == 1,
            "restart recovery did not reconstruct the committed settlement");
    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs immutable deal-fact and incremental-frontier checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_incremental_frontier();
        check_restart_recovery();
        std::cout << "managed deal settlement checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "managed deal settlement checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

/// \file managed_opening_allocation_store_test.cpp
/// \brief Verifies durable source-derived OPEN allocation frontiers.

#include "managed_opening_allocation.hpp"
#include "managed_opening_source_verifier.hpp"

#include "support/memory_journal_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

AccountKey account() { return {"demo", 42}; }
OperationKey operation() { return {account(), 71, 9}; }
constexpr ObservationWindow source_window() { return {1000, 2000}; }

class MemoryOpeningFactStore final : public DurableManagedOpeningFactStore {
public:
    ManagedOpeningFactCommitStatus commit(
        const ManagedOpeningDealFact &, const SingleWriterLease &) override {
        return ManagedOpeningFactCommitStatus::io_error;
    }

    ManagedOpeningFactLoadResult load(
        const AccountKey &, std::uint64_t) const override {
        return {ManagedOpeningFactLoadStatus::not_found, std::nullopt};
    }

    ManagedOpeningFactScanResult scan(const AccountKey &) const override {
        return {scan_status, facts};
    }

    ManagedOpeningFactScanStatus scan_status = ManagedOpeningFactScanStatus::complete;
    std::vector<ManagedOpeningDealFact> facts;
};

class TestLease final : public SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &candidate) const override {
        if (!held || candidate != account())
            return std::nullopt;
        return token;
    }

    bool with_fencing_capability(
        const AccountKey &candidate, std::uint64_t candidate_token,
        const std::function<void()> &publication) const override {
        if (!held || candidate != account() || candidate_token != token)
            return false;
        publication();
        return true;
    }

    bool held = true;
    std::uint64_t token = 17;
};

Mt5DealSnapshot deal(std::uint64_t ticket, double volume) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = 3001;
    value.position_id = 88001;
    value.type = 0;
    value.entry = static_cast<std::uint32_t>(BrokerDealEntry::in);
    value.volume = volume;
    value.time_msc = 1500;
    value.known_fields = kManagedOpeningRequiredDealFields;
    return value;
}

template <typename Callback>
void with_graph(const std::vector<Mt5DealSnapshot> &deals, Callback callback) {
    ObservationGraph graph(account());
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::history_deals;
    baseline_batch.history_deals_window = source_window();
    require(graph.apply(baseline_batch).accepted(), "baseline graph was not accepted");
    const auto baseline = capture_reconciliation_baseline(graph);
    ObservationBatch post_dispatch_batch = baseline_batch;
    post_dispatch_batch.history_deals = deals;
    require(graph.apply(post_dispatch_batch).accepted(),
            "post-dispatch history graph was not accepted");
    callback(graph, baseline);
}

ManagedOpeningDealFact fact_for(const ObservationGraph &graph,
                                std::uint64_t deal_ticket, double volume) {
    const auto source_revision = graph.domain_revision(ObservationDomain::history_deals);
    ManagedOpeningDealObservation observation{
        operation(), 2, 3001, deal_ticket, 88001, kManagedOpeningRequiredDealFields,
        BrokerPositionDirection::buy, BrokerDealEntry::in, 1500, volume, 0.01,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, source_revision,
        graph.instance_id(), source_revision, source_window().from_msc,
        source_window().to_msc};
    const ManagedOpeningExecutionRequest request{
        operation(), OperationKind::open, 2, 3001, 88001};
    const auto result = ManagedOpeningDealFact::from_observation(request, observation);
    require(result.has_value(), "valid opening fact was rejected");
    return *result;
}

OperationRecord journal_record(const ReconciliationBaseline &baseline,
                               std::uint64_t requested_volume = 5) {
    OperationRecord record;
    record.key = operation();
    record.operation_kind = OperationKind::open;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = OperationState::accepted;
    record.journal_state = JournalState::result_persisted;
    record.revision = 3;
    record.result_source_revision = 2;
    record.fencing_token = 17;
    record.reconciliation_descriptor = ReconciliationDescriptor{
        account(), baseline,
        {expect_reconciliation_transition(
            ReconciliationPredicateKind::history_deal_present, std::nullopt, false,
            ReconciliationTransition::absent_to_present, 70, source_window())},
        OperationState::filled, operation().trade_id, operation().operation_id,
        requested_volume};
    record.reconciliation_bindings = {{70, 5001}};
    require(record.valid(), "opening journal fixture is invalid");
    return record;
}

ManagedOpeningAllocationEntry make_entry(
    const VerifiedManagedOpeningSourceFact &proof,
    const std::vector<ManagedOpeningDealFact> &source_facts) {
    require(!source_facts.empty(), "opening allocation fixture has no facts");
    auto frontier = ManagedOpeningExecutionFrontier::create(source_facts);
    require(frontier.has_value(), "opening allocation frontier fixture is invalid");
    const auto &fact = frontier->entries().front();
    ManagedOpeningAllocationEntry entry;
    entry.account = proof.operation_key().account;
    entry.key.operation_key = proof.operation_key();
    entry.key.operation_revision = proof.operation_revision();
    for (const auto &candidate : frontier->entries())
        entry.key.frontier_revision =
            (std::max)(entry.key.frontier_revision,
                       candidate.admission_receipt().source_revision);
    entry.position_identifier = proof.position_identifier();
    entry.order_ticket = fact.order_ticket();
    entry.direction = fact.direction();
    entry.quantity_scale = fact.quantity().canonical_scale();
    entry.broker_step_units = fact.quantity().broker_step_units();
    entry.managed_logical_units_per_step =
        fact.quantity().managed_logical_units() / fact.quantity().integer_step_count();
    entry.cumulative_broker_units = frontier->cumulative_broker_units();
    entry.cumulative_managed_logical_units =
        frontier->cumulative_managed_logical_units();
    entry.evidence_digest = proof.evidence_digest();
    for (const auto &candidate : frontier->entries()) {
        const auto &receipt = candidate.admission_receipt();
        entry.fact_references.push_back({candidate.deal_ticket(), receipt.source_revision,
                                         receipt.graph_instance_id,
                                         receipt.history_coverage_revision,
                                         ManagedOpeningDealFact::k_schema_version});
    }
    require(entry.valid(), "opening allocation fixture is invalid");
    return entry;
}

std::filesystem::path test_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("mt5bridge-managed-opening-allocation-" + std::to_string(stamp));
}

std::filesystem::path allocation_record_path(const std::filesystem::path &directory) {
    for (const auto &item : std::filesystem::recursive_directory_iterator(directory))
        if (item.path().filename().wstring().rfind(L"alloc-", 0) == 0 &&
            item.path().extension() == L".bin")
            return item.path();
    throw std::runtime_error("allocation record was not published");
}

void durable_prepare_publish_restart_and_replay() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts.push_back(fact_for(graph, 5001, 0.05));
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        const auto proof_result = verifier.verify({operation(), OperationKind::open, 3, 88001});
        require(proof_result.found(), "source verifier did not produce a proof");
        const auto entry = make_entry(*proof_result.proof, facts.facts);
        auto unknown_schema = entry;
        unknown_schema.fact_references.front().schema_version = 99;
        require(!unknown_schema.valid(), "unknown allocation fact schema was accepted");

        const auto directory = test_directory();
        std::filesystem::remove_all(directory);
        try {
            WindowsFileManagedOpeningAllocationStore store(directory);
            require(store.ready(), "opening allocation store did not open");
            require(store.prepare(entry, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                    "opening allocation preparation was not committed");
            const auto preparation = store.load_preparation(entry.key);
            require(preparation.found() && preparation.record->entry == entry,
                    "opening allocation preparation did not survive reload");
            auto altered_preparation = *preparation.record;
            ++altered_preparation.entry.order_ticket;
            require(store.publish(altered_preparation, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::conflict,
                    "changed preparation payload was not rejected as a conflict");
            require(store.prepare(entry, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::already_committed,
                    "opening allocation preparation replay was not idempotent");
            require(store.publish(*preparation.record, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                    "opening allocation was not published");
            require(store.publish(*preparation.record, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::already_committed,
                    "published allocation replay was not idempotent");

            WindowsFileManagedOpeningAllocationStore restarted(directory);
            const auto recovered = restarted.load(entry.key);
            require(recovered.found() && recovered.record->operator==(entry),
                    "published allocation did not survive restart");
            const auto scan = restarted.scan(account(), journal, facts);
            require(scan.complete() && scan.records.size() == 1 && scan.preparations.empty(),
                    "allocation scan did not recover the committed frontier");
            require(restarted.scan({"foreign", 77}, journal, facts).complete(),
                    "unknown account scan did not return an empty complete result");

            facts.facts.clear();
            require(restarted.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "allocation with a missing source fact was accepted");

            facts.facts.push_back(fact_for(graph, 5001, 0.05));
            const auto path = allocation_record_path(directory);
            std::fstream record(path, std::ios::binary | std::ios::in | std::ios::out);
            require(static_cast<bool>(record), "allocation record could not be reopened");
            record.seekg(-1, std::ios::end);
            char last = 0;
            record.read(&last, 1);
            record.seekp(-1, std::ios::end);
            last = static_cast<char>(last ^ 0x01);
            record.write(&last, 1);
            record.flush();
            require(restarted.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "corrupted allocation record was accepted");
        } catch (...) {
            std::filesystem::remove_all(directory);
            throw;
        }
        std::filesystem::remove_all(directory);

        const auto publisher_directory = test_directory();
        std::filesystem::remove_all(publisher_directory);
        try {
            WindowsFileManagedOpeningAllocationStore store(publisher_directory);
            ManagedOpeningAllocationPublisher publisher(
                verifier, facts, store, journal, lease);
            const auto prepared = publisher.prepare(
                {operation(), OperationKind::open, 3, 88001});
            require(prepared.status == ManagedOpeningAllocationPublicationStatus::prepared &&
                        prepared.record,
                    "source-derived publisher did not prepare an allocation");
            const auto published = publisher.publish(
                prepared.record->key, {operation(), OperationKind::open, 3, 88001});
            require(published.status == ManagedOpeningAllocationPublicationStatus::published,
                    "source-derived publisher did not publish an allocation");
            const auto replayed = publisher.publish(
                prepared.record->key, {operation(), OperationKind::open, 3, 88001});
            require(replayed.status ==
                        ManagedOpeningAllocationPublicationStatus::already_published,
                    "source-derived publisher replay was not idempotent");
        } catch (...) {
            std::filesystem::remove_all(publisher_directory);
            throw;
        }
        std::filesystem::remove_all(publisher_directory);
    });
}

void rejects_missing_source_and_stale_lease() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "missing source was accepted by verifier");
        lease.held = false;
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::lease_not_held,
                "stale lease was not rejected");
    });
}

void preserves_historical_frontiers_when_a_partial_fill_arrives() {
    ObservationGraph graph(account());
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::history_deals;
    baseline_batch.history_deals_window = source_window();
    require(graph.apply(baseline_batch).accepted(), "progression baseline was not accepted");
    const auto baseline = capture_reconciliation_baseline(graph);

    ObservationBatch first_fill = baseline_batch;
    first_fill.history_deals = {deal(5001, 0.05)};
    require(graph.apply(first_fill).accepted(), "first opening fill was not accepted");

    MemoryOpeningFactStore facts;
    facts.facts.push_back(fact_for(graph, 5001, 0.05));
    mt5bridge_test_support::MemoryJournalStore journal;
    journal.records.emplace(operation(), journal_record(baseline, 8));
    TestLease lease;
    ManagedOpeningSourceVerifier verifier(
        journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    const auto proof_a = verifier.verify({operation(), OperationKind::open, 3, 88001});
    require(proof_a.found(), "first opening frontier was not verified");
    const auto entry_a = make_entry(*proof_a.proof, facts.facts);

    const auto directory = test_directory();
    std::filesystem::remove_all(directory);
    try {
        WindowsFileManagedOpeningAllocationStore store(directory);
        require(store.prepare(entry_a, *proof_a.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed,
                "first opening frontier was not prepared");
        const auto prep_a = store.load_preparation(entry_a.key);
        require(prep_a.found(), "first opening frontier preparation was not recovered");
        require(store.publish(*prep_a.record, *proof_a.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed,
                "first opening frontier was not published");

        ObservationBatch second_fill = baseline_batch;
        second_fill.history_deals = {deal(5001, 0.05), deal(5002, 0.03)};
        require(graph.apply(second_fill).accepted(), "second opening fill was not accepted");
        facts.facts.push_back(fact_for(graph, 5002, 0.03));
        const auto proof_b = verifier.verify({operation(), OperationKind::open, 3, 88001});
        require(proof_b.found() && proof_b.proof->deal_tickets().size() == 2,
                "second opening frontier was not verified");
        const auto entry_b = make_entry(*proof_b.proof, facts.facts);
        require(entry_b.key.frontier_revision > entry_a.key.frontier_revision &&
                    entry_b.fact_references.size() == 2,
                "second opening frontier was not cumulative");
        require(store.prepare(entry_b, *proof_b.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed,
                "second opening frontier was not prepared");
        const auto prep_b = store.load_preparation(entry_b.key);
        require(prep_b.found(), "second opening frontier preparation was not recovered");
        require(store.publish(*prep_b.record, *proof_b.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed,
                "second opening frontier was not published");

        const auto scan = store.scan(account(), journal, facts);
        require(scan.complete() && scan.records.size() == 2,
                "historical opening frontier was rewritten or rejected");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    durable_prepare_publish_restart_and_replay();
    rejects_missing_source_and_stale_lease();
    preserves_historical_frontiers_when_a_partial_fill_arrives();
    return 0;
}

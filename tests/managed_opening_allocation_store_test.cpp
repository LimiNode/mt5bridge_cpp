/// \file managed_opening_allocation_store_test.cpp
/// \brief Verifies durable source-derived OPEN allocation frontiers.

#include "managed_opening_allocation.hpp"
#include "managed_opening_source_verifier.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

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
                                std::uint64_t deal_ticket, double volume,
                                std::uint64_t operation_revision = 2) {
    const auto source_revision = graph.domain_revision(ObservationDomain::history_deals);
    ManagedOpeningDealObservation observation{
        operation(), operation_revision, 3001, deal_ticket, 88001,
        kManagedOpeningRequiredDealFields,
        BrokerPositionDirection::buy, BrokerDealEntry::in, 1500, volume, 0.01,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, source_revision,
        graph.instance_id(), source_revision, source_window().from_msc,
        source_window().to_msc};
    const ManagedOpeningExecutionRequest request{
        operation(), OperationKind::open, operation_revision, 3001, 88001};
    const auto result = ManagedOpeningDealFact::from_observation(request, observation);
    require(result.has_value(), "valid opening fact was rejected");
    return *result;
}

OperationRecord journal_record(const ReconciliationBaseline &baseline,
                               std::uint64_t requested_volume = 5,
                               std::uint64_t revision = 3,
                               std::uint64_t result_source_revision = 2) {
    OperationRecord record;
    record.key = operation();
    record.operation_kind = OperationKind::open;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = OperationState::accepted;
    record.journal_state = JournalState::result_persisted;
    record.revision = revision;
    record.result_source_revision = result_source_revision;
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

std::filesystem::path record_path_with_prefix(const std::filesystem::path &directory,
                                              const wchar_t *prefix) {
    for (const auto &item : std::filesystem::recursive_directory_iterator(directory))
        if (item.path().filename().wstring().rfind(prefix, 0) == 0 &&
            item.path().extension() == L".bin")
            return item.path();
    throw std::runtime_error("requested allocation record was not found");
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
            require(altered_preparation.valid(), "altered preparation became structurally invalid");
            const auto altered_status = store.publish(
                altered_preparation, journal, facts, lease, verifier,
                {operation(), OperationKind::open, 3, 88001});
            require(altered_status == ManagedOpeningAllocationCommitStatus::conflict,
                    "changed preparation payload was not rejected as a conflict");
            require(store.prepare(entry, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::already_committed,
                    "opening allocation preparation replay was not idempotent");
            require(store.publish(*preparation.record, journal, facts, lease, verifier,
                                  {operation(), OperationKind::open, 3, 88001}) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                    "opening allocation was not published");
            require(store.publish(*preparation.record, journal, facts, lease, verifier,
                                  {operation(), OperationKind::open, 3, 88001}) ==
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

void rejects_stale_source_proof_at_low_level_publication() {
    ObservationGraph graph(account());
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::history_deals;
    baseline_batch.history_deals_window = source_window();
    require(graph.apply(baseline_batch).accepted(),
            "stale-proof baseline was not accepted");
    const auto baseline = capture_reconciliation_baseline(graph);
    ObservationBatch observed = baseline_batch;
    observed.history_deals = {deal(5001, 0.05)};
    require(graph.apply(observed).accepted(), "stale-proof source was not accepted");

    MemoryOpeningFactStore facts;
    facts.facts.push_back(fact_for(graph, 5001, 0.05));
    mt5bridge_test_support::MemoryJournalStore journal;
    journal.records.emplace(operation(), journal_record(baseline));
    TestLease lease;
    ManagedOpeningSourceVerifier verifier(
        journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    const auto proof = verifier.verify({operation(), OperationKind::open, 3, 88001});
    require(proof.found(), "stale-proof fixture did not produce an initial proof");
    const auto entry = make_entry(*proof.proof, facts.facts);

    const auto directory = test_directory();
    std::filesystem::remove_all(directory);
    try {
        WindowsFileManagedOpeningAllocationStore store(directory);
        require(store.prepare(entry, *proof.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed,
                "stale-proof preparation was not committed");

        ObservationBatch contradiction = baseline_batch;
        contradiction.history_deals = {};
        require(graph.apply(contradiction).accepted(),
                "stale-proof contradiction sample was not accepted");
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "fresh B3B verification accepted a removed deal");
        require(store.publish(ManagedOpeningAllocationPreparation{entry}, journal, facts, lease,
                              verifier,
                              {operation(), OperationKind::open, 3, 88001}) ==
                    ManagedOpeningAllocationCommitStatus::invalid_source,
                "low-level publish accepted a stale B3B proof");
        require(!store.load(entry.key).found(),
                "stale B3B proof created a committed allocation");

        WindowsFileManagedOpeningAllocationStore restarted(directory);
        const auto scan = restarted.scan(account(), journal, facts);
        require(scan.complete() && scan.records.empty() && scan.preparations.size() == 1,
                "stale-proof restart did not retain only the pending preparation");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

void rejects_noncanonical_reserved_paths_and_keeps_pending_separate() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts.push_back(fact_for(graph, 5001, 0.05));
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        const auto proof = verifier.verify({operation(), OperationKind::open, 3, 88001});
        require(proof.found(), "namespace fixture did not produce a source proof");
        const auto entry = make_entry(*proof.proof, facts.facts);
        const auto directory = test_directory();
        std::filesystem::remove_all(directory);
        try {
            WindowsFileManagedOpeningAllocationStore store(directory);
            require(store.prepare(entry, *proof.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                    "namespace preparation was not committed");
            const auto preparation_path = record_path_with_prefix(directory, L"prep-");
            const auto preparation_alias = preparation_path.parent_path() / L"prep-alias.bin";
            std::filesystem::copy_file(preparation_path, preparation_alias);
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "preparation alias was accepted");
            std::filesystem::remove(preparation_alias);

            require(store.publish(ManagedOpeningAllocationPreparation{entry}, journal, facts,
                                  lease, verifier,
                                  {operation(), OperationKind::open, 3, 88001}) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                    "namespace allocation was not published");
            const auto allocation_path = allocation_record_path(directory);
            const auto allocation_alias = allocation_path.parent_path() / L"alloc-alias.bin";
            std::filesystem::copy_file(allocation_path, allocation_alias);
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "allocation alias was accepted");
            std::filesystem::remove(allocation_alias);

            const auto allocation_name = allocation_path.filename().wstring();
            const auto pending_alias = allocation_path.parent_path() /
                (L"prep-" + allocation_name.substr(6));
            std::filesystem::copy_file(allocation_path, pending_alias);
            const auto pending_scan = store.scan(account(), journal, facts);
            require(pending_scan.complete() && pending_scan.records.size() == 1 &&
                        pending_scan.preparations.size() == 1,
                    "prep plus alloc was counted as two committed allocations");
            std::filesystem::remove(pending_alias);

            const auto reserved_directory = allocation_path.parent_path() / L"alloc-directory.bin";
            std::filesystem::create_directory(reserved_directory);
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "reserved allocation directory was accepted");
        } catch (...) {
            std::filesystem::remove_all(directory);
            throw;
        }
        std::filesystem::remove_all(directory);
    });
}

void recovers_through_real_filesystem_sources() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        const auto root = test_directory();
        std::filesystem::remove_all(root);
        try {
            WindowsSingleWriterLease lease(root / "lease", account());
            require(lease.ready(), "real filesystem lease did not open");
            WindowsFileJournalStore journal_store(root / "journal");
            WindowsFileManagedOpeningFactStore fact_store(root / "facts");
            WindowsFileManagedOpeningAllocationStore allocation_store(root / "allocations");
            require(journal_store.ready() && fact_store.ready() && allocation_store.ready(),
                    "real filesystem stores did not open");

            auto record = journal_record(baseline, 5, 1, 1);
            const auto token = lease.held_fencing_token(account());
            require(token.has_value(), "real filesystem lease token was unavailable");
            record.fencing_token = *token;
            require(record.valid(), "real filesystem journal fixture is invalid");
            require(journal_store.commit(record, std::nullopt) ==
                        StoreCommitStatus::committed,
                    "durable journal result was not committed");

            const auto durable_fact = fact_for(graph, 5001, 0.05, 1);
            require(fact_store.commit(durable_fact, lease) ==
                        ManagedOpeningFactCommitStatus::committed,
                    "durable B3A fact was not committed");
            require(fact_store.scan(account()).complete(),
                    "durable B3A fact did not survive its first scan");

            ManagedOpeningSourceVerifier verifier(
                journal_store, fact_store, graph,
                ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
            const ManagedOpeningProvenanceRequest request{
                operation(), OperationKind::open, 1, 88001};
            const auto proof = verifier.verify(request);
            require(proof.found(), "real filesystem B3B verification failed");
            ManagedOpeningAllocationPublisher publisher(
                verifier, fact_store, allocation_store, journal_store, lease);
            const auto prepared = publisher.prepare(request);
            require(prepared.status == ManagedOpeningAllocationPublicationStatus::prepared &&
                        prepared.record,
                    "real filesystem B3C preparation failed");

            WindowsFileManagedOpeningAllocationStore pending_restart(root / "allocations");
            const auto pending = pending_restart.scan(account(), journal_store, fact_store);
            require(pending.complete() && pending.records.empty() &&
                        pending.preparations.size() == 1,
                    "pending preparation was not recovered from the filesystem");

            ManagedOpeningAllocationPublisher recovered_publisher(
                verifier, fact_store, pending_restart, journal_store, lease);
            const auto published = recovered_publisher.publish(prepared.record->key, request);
            require(published.status == ManagedOpeningAllocationPublicationStatus::published,
                    "recovered preparation was not published");

            WindowsFileJournalStore restarted_journal(root / "journal");
            WindowsFileManagedOpeningFactStore restarted_facts(root / "facts");
            WindowsFileManagedOpeningAllocationStore restarted_allocations(root / "allocations");
            require(restarted_journal.load(operation()).found() &&
                        restarted_facts.load(account(), 5001).found(),
                    "durable source records did not survive restart");
            const auto replay_scan =
                restarted_allocations.scan(account(), restarted_journal, restarted_facts);
            require(replay_scan.complete() && replay_scan.records.size() == 1 &&
                        replay_scan.preparations.empty(),
                    "real filesystem allocation replay was not exact");
            ManagedOpeningSourceVerifier restarted_verifier(
                restarted_journal, restarted_facts, graph,
                ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
            ManagedOpeningAllocationPublisher replay_publisher(
                restarted_verifier, restarted_facts, restarted_allocations,
                restarted_journal, lease);
            const auto replayed = replay_publisher.publish(prepared.record->key, request);
            require(replayed.status == ManagedOpeningAllocationPublicationStatus::already_published,
                    "real filesystem allocation replay was not idempotent");
        } catch (...) {
            std::filesystem::remove_all(root);
            throw;
        }
        std::filesystem::remove_all(root);
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
        require(store.publish(*prep_a.record, journal, facts, lease, verifier,
                              {operation(), OperationKind::open, 3, 88001}) ==
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
        require(store.publish(*prep_b.record, journal, facts, lease, verifier,
                              {operation(), OperationKind::open, 3, 88001}) ==
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
    rejects_stale_source_proof_at_low_level_publication();
    rejects_noncanonical_reserved_paths_and_keeps_pending_separate();
    recovers_through_real_filesystem_sources();
    preserves_historical_frontiers_when_a_partial_fill_arrives();
    return 0;
}

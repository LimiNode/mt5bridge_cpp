/// \file managed_opening_allocation_store_test.cpp
/// \brief Verifies durable source-derived OPEN allocation frontiers.

#include "managed_opening_allocation.hpp"
#include "managed_opening_source_verifier.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include "support/memory_journal_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
            auto alias = entry;
            alias.key.frontier_revision += 100;
            require(store.prepare(alias, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::already_committed &&
                        !store.load_preparation(alias.key).found(),
                    "source-equivalent alias created a second preparation");
            require(store.publish(ManagedOpeningAllocationPreparation{alias}, journal, facts,
                                  lease, verifier,
                                  {operation(), OperationKind::open, 3, 88001}) ==
                        ManagedOpeningAllocationCommitStatus::already_committed,
                    "source-equivalent alias created a second committed allocation");
            require(store.prepare(entry, *proof_result.proof, journal, facts, lease) ==
                        ManagedOpeningAllocationCommitStatus::already_committed &&
                        !store.load_preparation(entry.key).found(),
                    "preparing an already-published allocation created a pending manifest");
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
            const auto prepared_again = publisher.prepare(
                {operation(), OperationKind::open, 3, 88001});
            require(prepared_again.status ==
                            ManagedOpeningAllocationPublicationStatus::already_prepared &&
                        prepared_again.record &&
                        !store.load_preparation(prepared.record->key).found(),
                    "publisher re-prepared an already-published allocation");
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
            const auto orphan_preparation =
                preparation_path.parent_path() / L"prep-orphan.bin.tmp";
            {
                std::ofstream orphan(orphan_preparation, std::ios::binary);
                require(static_cast<bool>(orphan), "orphan preparation could not be created");
            }
            auto orphan_scan = store.scan(account(), journal, facts);
            require(orphan_scan.complete() && orphan_scan.preparations.size() == 1,
                    "empty orphan preparation blocked recovery");
            std::filesystem::copy_file(
                preparation_path, orphan_preparation,
                std::filesystem::copy_options::overwrite_existing);
            orphan_scan = store.scan(account(), journal, facts);
            require(orphan_scan.complete() && orphan_scan.preparations.size() == 1,
                    "complete orphan preparation blocked recovery");
            const auto malformed_temporary =
                preparation_path.parent_path() / L"prep-invalid.tmp";
            {
                std::ofstream malformed(malformed_temporary, std::ios::binary);
                require(static_cast<bool>(malformed),
                        "malformed temporary preparation could not be created");
            }
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "non-canonical temporary preparation was ignored");
            require(std::filesystem::remove(malformed_temporary),
                    "malformed temporary preparation could not be removed");
            const auto temporary_directory =
                preparation_path.parent_path() / L"prep-directory.bin.tmp";
            require(std::filesystem::create_directory(temporary_directory),
                    "temporary preparation directory could not be created");
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "temporary preparation directory bypassed safety checks");
            require(std::filesystem::remove_all(temporary_directory) == 1,
                    "temporary preparation directory could not be removed");
            ManagedOpeningAllocationPublisher publisher(
                verifier, facts, store, journal, lease);
            require(publisher.prepare({operation(), OperationKind::open, 3, 88001}).status ==
                        ManagedOpeningAllocationPublicationStatus::already_prepared,
                    "publisher prepare was blocked by orphan preparation");
            require(std::filesystem::remove(orphan_preparation),
                    "orphan preparation could not be removed");
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
            const auto orphan_allocation =
                allocation_path.parent_path() / L"alloc-orphan.bin.tmp";
            std::filesystem::copy_file(
                allocation_path, orphan_allocation,
                std::filesystem::copy_options::overwrite_existing);
            require(publisher.publish(entry.key, {operation(), OperationKind::open, 3, 88001}).status ==
                        ManagedOpeningAllocationPublicationStatus::already_published,
                    "publisher publish was blocked by orphan allocation");
            const auto allocation_orphan_scan = store.scan(account(), journal, facts);
            require(allocation_orphan_scan.complete() && allocation_orphan_scan.records.size() == 1,
                    "orphan allocation changed durable scan");
            require(std::filesystem::remove(orphan_allocation),
                    "orphan allocation could not be removed");
            const auto allocation_directory =
                allocation_path.parent_path() / L"alloc-directory.bin.tmp";
            require(std::filesystem::create_directory(allocation_directory),
                    "temporary allocation directory could not be created");
            require(store.scan(account(), journal, facts).status ==
                        ManagedOpeningAllocationScanStatus::invalid_record,
                    "temporary allocation directory bypassed safety checks");
            require(std::filesystem::remove_all(allocation_directory) == 1,
                    "temporary allocation directory could not be removed");
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

        require(store.publish(ManagedOpeningAllocationPreparation{entry_a}, journal, facts,
                              lease, verifier,
                              {operation(), OperationKind::open, 3, 88001}) ==
                    ManagedOpeningAllocationCommitStatus::already_committed,
                "historical opening frontier direct replay was not idempotent");
        ManagedOpeningAllocationPublisher publisher(
            verifier, facts, store, journal, lease);
        const auto replayed = publisher.publish(
            entry_a.key, {operation(), OperationKind::open, 3, 88001});
        require(replayed.status ==
                    ManagedOpeningAllocationPublicationStatus::already_published,
                "historical opening frontier publisher replay was not idempotent");

        ObservationBatch contradiction = baseline_batch;
        contradiction.history_deals = {deal(5002, 0.03)};
        require(graph.apply(contradiction).accepted(),
                "historical source contradiction was not accepted by the graph");
        require(store.publish(ManagedOpeningAllocationPreparation{entry_a}, journal, facts,
                              lease, verifier,
                              {operation(), OperationKind::open, 3, 88001}) ==
                    ManagedOpeningAllocationCommitStatus::invalid_source,
                "historical source rewrite was accepted during replay");

        const auto scan = store.scan(account(), journal, facts);
        require(scan.complete() && scan.records.size() == 2,
                "historical opening frontier was rewritten or rejected");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

/// \brief Verifies source revision ordering independently for each graph epoch.
/// \note Numeric revisions from different observation graphs are not comparable.
void preserves_cross_graph_frontier_history() {
    ObservationBatch empty_history;
    empty_history.account = account();
    empty_history.observed_domains = ObservationDomain::history_deals;
    empty_history.history_deals_window = source_window();
    const auto baseline_batch = empty_history;

    ObservationGraph graph_a(account());
    require(graph_a.apply(empty_history).accepted(), "graph A baseline was not accepted");
    const auto baseline = capture_reconciliation_baseline(graph_a);
    empty_history.history_deals = {deal(5001, 0.05)};
    require(graph_a.apply(empty_history).accepted(), "graph A fill was not accepted");

    MemoryOpeningFactStore facts;
    facts.facts.push_back(fact_for(graph_a, 5001, 0.05));
    mt5bridge_test_support::MemoryJournalStore journal;
    journal.records.emplace(operation(), journal_record(baseline, 10));
    TestLease lease;
    const ManagedOpeningProvenanceRequest request{
        operation(), OperationKind::open, 3, 88001};
    ManagedOpeningSourceVerifier verifier_a(
        journal, facts, graph_a, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    const auto proof_a = verifier_a.verify(request);
    require(proof_a.found(), "graph A proof was not verified");
    const auto entry_a = make_entry(*proof_a.proof, facts.facts);

    const auto directory = test_directory();
    std::filesystem::remove_all(directory);
    try {
        WindowsFileManagedOpeningAllocationStore store(directory);
        require(store.prepare(entry_a, *proof_a.proof, journal, facts, lease) ==
                    ManagedOpeningAllocationCommitStatus::committed &&
                    store.publish(ManagedOpeningAllocationPreparation{entry_a}, journal, facts,
                                  lease, verifier_a, request) ==
                        ManagedOpeningAllocationCommitStatus::committed,
                "graph A frontier was not published");

        ObservationGraph graph_b(account());
        require(graph_b.instance_id() != graph_a.instance_id(),
                "independent graph epochs reused their identity");
        ObservationBatch first_b = baseline_batch;
        first_b.history_deals = {deal(5001, 0.05), deal(5002, 0.03)};
        require(graph_b.apply(first_b).accepted(), "graph B first fill was not accepted");
        facts.facts.push_back(fact_for(graph_b, 5002, 0.03));
        ManagedOpeningSourceVerifier verifier_b(
            journal, facts, graph_b, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        ManagedOpeningAllocationPublisher publisher_b(
            verifier_b, facts, store, journal, lease);
        const auto prepared_b = publisher_b.prepare(request);
        require(prepared_b.status == ManagedOpeningAllocationPublicationStatus::prepared &&
                    prepared_b.record &&
                    publisher_b.publish(prepared_b.record->key, request).status ==
                        ManagedOpeningAllocationPublicationStatus::published,
                "graph B first frontier was not published");

        ObservationBatch second_b = baseline_batch;
        second_b.history_deals = {deal(5001, 0.05), deal(5002, 0.03), deal(5003, 0.02)};
        require(graph_b.apply(second_b).accepted(), "graph B second fill was not accepted");
        facts.facts.push_back(fact_for(graph_b, 5003, 0.02));
        const auto historical = store.scan(account(), journal, facts);
        require(historical.complete() && historical.records.size() == 2,
                "cross-graph historical frontier was rejected by a foreign revision");

        const auto prepared_c = publisher_b.prepare(request);
        require(prepared_c.status == ManagedOpeningAllocationPublicationStatus::prepared &&
                    prepared_c.record &&
                    publisher_b.publish(prepared_c.record->key, request).status ==
                        ManagedOpeningAllocationPublicationStatus::published,
                "graph B second frontier was not publishable");
        const auto final = store.scan(account(), journal, facts);
        require(final.complete() && final.records.size() == 3 && final.preparations.empty(),
                "cross-graph frontier progression did not converge");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

/// \brief Replays a three-frontier supersession chain after a process restart.
void recovers_one_durable_successor_after_multiple_supersessions() {
    const auto root = test_directory();
    std::filesystem::remove_all(root);
    try {
        std::optional<ManagedOpeningAllocationKey> key_a;
        std::optional<ManagedOpeningAllocationKey> key_b;
        std::optional<ManagedOpeningAllocationKey> key_c;
        {
            ObservationGraph graph(account());
            ObservationBatch history;
            history.account = account();
            history.observed_domains = ObservationDomain::history_deals;
            history.history_deals_window = source_window();
            require(graph.apply(history).accepted(), "multi-successor baseline failed");
            const auto baseline = capture_reconciliation_baseline(graph);
            const auto baseline_history = history;
            auto first_fill = baseline_history;
            first_fill.history_deals = {deal(5001, 0.05)};
            require(graph.apply(first_fill).accepted(), "multi-successor first fill failed");

            WindowsFileJournalStore journal(root / "journal");
            WindowsFileManagedOpeningFactStore facts(root / "facts");
            WindowsSingleWriterLease lease(root / "lease", account());
            WindowsFileManagedOpeningAllocationStore store(root / "allocations");
            auto record = journal_record(baseline, 10, 1, 1);
            const auto token = lease.held_fencing_token(account());
            require(token.has_value(), "multi-successor lease failed");
            record.fencing_token = *token;
            require(journal.commit(record, std::nullopt) == StoreCommitStatus::committed &&
                        facts.commit(fact_for(graph, 5001, 0.05, 1), lease) ==
                            ManagedOpeningFactCommitStatus::committed,
                    "multi-successor first sources were not durable");
            ManagedOpeningSourceVerifier verifier(
                journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
            ManagedOpeningAllocationPublisher publisher(verifier, facts, store, journal, lease);
            const ManagedOpeningProvenanceRequest request{
                operation(), OperationKind::open, 1, 88001};
            const auto a = publisher.prepare(request);
            require(a.status == ManagedOpeningAllocationPublicationStatus::prepared && a.record,
                    "multi-successor A was not prepared");
            key_a = a.record->key;

            auto second_fill = baseline_history;
            second_fill.history_deals = {deal(5001, 0.05), deal(5002, 0.03)};
            require(graph.apply(second_fill).accepted(), "multi-successor second fill failed");
            require(facts.commit(fact_for(graph, 5002, 0.03, 1), lease) ==
                        ManagedOpeningFactCommitStatus::committed,
                    "multi-successor second source was not durable");
            const auto b = publisher.prepare(request);
            require(b.status == ManagedOpeningAllocationPublicationStatus::prepared && b.record,
                    "multi-successor B was not prepared");
            key_b = b.record->key;

            auto third_fill = baseline_history;
            third_fill.history_deals = {
                deal(5001, 0.05), deal(5002, 0.03), deal(5003, 0.02)};
            require(graph.apply(third_fill).accepted(), "multi-successor third fill failed");
            require(facts.commit(fact_for(graph, 5003, 0.02, 1), lease) ==
                        ManagedOpeningFactCommitStatus::committed,
                    "multi-successor third source was not durable");
            const auto c = publisher.prepare(request);
            require(c.status == ManagedOpeningAllocationPublicationStatus::prepared && c.record,
                    "multi-successor C was not prepared");
            key_c = c.record->key;

            require(publisher.publish(*key_b, request).status ==
                        ManagedOpeningAllocationPublicationStatus::superseded,
                    "publishing B did not reuse durable C");
            require(publisher.publish(*key_a, request).status ==
                        ManagedOpeningAllocationPublicationStatus::superseded,
                    "publishing A did not reuse durable C");
            const auto pending = store.scan(account(), journal, facts);
            require(pending.complete() && pending.records.empty() &&
                        pending.preparations.size() == 1 &&
                        pending.preparations.front().entry.key == *key_c,
                    "multi-successor cleanup left a duplicate frontier");
        }

        WindowsFileJournalStore journal(root / "journal");
        WindowsFileManagedOpeningFactStore facts(root / "facts");
        WindowsSingleWriterLease lease(root / "lease", account());
        WindowsFileManagedOpeningAllocationStore store(root / "allocations");
        ObservationGraph graph(account());
        ObservationBatch current;
        current.account = account();
        current.observed_domains = ObservationDomain::history_deals;
        current.history_deals_window = source_window();
        current.history_deals = {deal(5001, 0.05), deal(5002, 0.03), deal(5003, 0.02)};
        require(graph.apply(current).accepted(), "multi-successor restart history failed");
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        ManagedOpeningAllocationPublisher publisher(verifier, facts, store, journal, lease);
        const auto recovered = store.scan(account(), journal, facts);
        require(recovered.complete() && recovered.records.empty() &&
                    recovered.preparations.size() == 1 &&
                    recovered.preparations.front().entry.key == *key_c,
                "multi-successor restart did not retain canonical C");
        require(publisher.publish(*key_c, {operation(), OperationKind::open, 1, 88001}).status ==
                    ManagedOpeningAllocationPublicationStatus::published,
                "canonical C was not publishable after restart");
        const auto final = store.scan(account(), journal, facts);
        require(final.complete() && final.records.size() == 1 && final.preparations.empty() &&
                    final.records.front().fact_references.size() == 3,
                "multi-successor restart created a duplicate allocation");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

/// \brief Exercises each durable boundary without changing production storage seams.
void preserves_superseded_publication_intent() {
    enum class CrashPoint { before_successor, pending_successor, committed_successor,
                            successor_write_error, predecessor_cleanup_error,
                            successor_cleanup_error, canonical_successor_mismatch };
    for (const auto point : {CrashPoint::before_successor, CrashPoint::pending_successor,
                             CrashPoint::committed_successor, CrashPoint::successor_write_error,
                             CrashPoint::predecessor_cleanup_error,
                             CrashPoint::successor_cleanup_error,
                             CrashPoint::canonical_successor_mismatch}) {
        const auto root = test_directory();
        try {
            {
                ObservationGraph graph(account());
                ObservationBatch history;
                history.account = account();
                history.observed_domains = ObservationDomain::history_deals;
                history.history_deals_window = source_window();
                require(graph.apply(history).accepted(), "supersession baseline failed");
                const auto baseline = capture_reconciliation_baseline(graph);
                history.history_deals = {deal(5001, 0.05)};
                require(graph.apply(history).accepted(), "supersession first fill failed");
                WindowsFileJournalStore journal(root / "journal");
                WindowsFileManagedOpeningFactStore facts(root / "facts");
                WindowsSingleWriterLease lease(root / "lease", account());
                WindowsFileManagedOpeningAllocationStore store(root / "allocations");
                auto record = journal_record(baseline, 8, 1, 1);
                const auto token = lease.held_fencing_token(account());
                require(token.has_value(), "supersession lease failed");
                record.fencing_token = *token;
                require(journal.commit(record, std::nullopt) == StoreCommitStatus::committed &&
                            facts.commit(fact_for(graph, 5001, 0.05, 1), lease) ==
                                ManagedOpeningFactCommitStatus::committed,
                        "supersession sources were not durable");
                ManagedOpeningSourceVerifier verifier(
                    journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
                ManagedOpeningAllocationPublisher publisher(verifier, facts, store, journal, lease);
                const ManagedOpeningProvenanceRequest request{
                    operation(), OperationKind::open, 1, 88001};
                const auto a = publisher.prepare(request);
                require(a.status == ManagedOpeningAllocationPublicationStatus::prepared && a.record,
                        "supersession A was not prepared");
                const auto path_a = record_path_with_prefix(root / "allocations", L"prep-");
                const auto initial = store.scan(account(), journal, facts);
                require(initial.complete() && initial.records.empty() &&
                            initial.preparations.size() == 1,
                        "intent was missing before the second fill");
                history.history_deals.push_back(deal(5002, 0.03));
                require(graph.apply(history).accepted(), "supersession second fill failed");
                require(facts.commit(fact_for(graph, 5002, 0.03, 1), lease) ==
                            ManagedOpeningFactCommitStatus::committed,
                        "second-fill fact was not durable");
                const auto expanded_proof = verifier.verify(request);
                require(expanded_proof.found(), "expanded source proof failed");
                const auto still_pending = store.scan(account(), journal, facts);
                require(still_pending.complete() && still_pending.records.empty() &&
                            still_pending.preparations.size() == 1,
                        "fresh extended proof alone erased pending intent");

                std::optional<ManagedOpeningAllocationEntry> b;
                std::filesystem::path path_b;
                std::filesystem::path blocked_temporary;
                if (point != CrashPoint::before_successor &&
                    point != CrashPoint::predecessor_cleanup_error) {
                    const auto prepared_b = publisher.prepare(request);
                    require(prepared_b.status == ManagedOpeningAllocationPublicationStatus::prepared &&
                                prepared_b.record,
                            "successor fixture was not durable");
                    b = prepared_b.record;
                    for (const auto &item : std::filesystem::recursive_directory_iterator(
                             root / "allocations"))
                        if (item.path() != path_a &&
                            item.path().filename().wstring().rfind(L"prep-", 0) == 0 &&
                            item.path().extension() == L".bin")
                            path_b = item.path();
                    require(!path_b.empty(), "successor manifest path was not found");
                    if (point == CrashPoint::successor_write_error) {
                        require(std::filesystem::remove(path_b), "write-error fixture removal failed");
                        blocked_temporary = std::filesystem::path(path_b.wstring() + L".tmp");
                        require(std::filesystem::create_directory(blocked_temporary),
                                "successor temporary write was not blocked");
                    } else if (point == CrashPoint::committed_successor) {
                        require(publisher.publish(b->key, request).status ==
                                    ManagedOpeningAllocationPublicationStatus::published,
                                "successor allocation was not committed");
                    } else if (point == CrashPoint::canonical_successor_mismatch) {
                        auto alias = *b;
                        ++alias.key.frontier_revision;
                        require(store.prepare(alias, *expanded_proof.proof, journal, facts, lease) ==
                                    ManagedOpeningAllocationCommitStatus::already_committed &&
                                    !store.load_preparation(alias.key).found(),
                                "source-equivalent alias bypassed durable identity guard");
                    }
                }

                if (point == CrashPoint::predecessor_cleanup_error) {
                    // Deny DELETE only: publication can read A and persist B.
                    const auto held = CreateFileW(path_a.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    require(held != INVALID_HANDLE_VALUE, "could not block predecessor cleanup");
                    const auto status = publisher.publish(a.record->key, request).status;
                    CloseHandle(held);
                    require(status == ManagedOpeningAllocationPublicationStatus::io_error,
                            "predecessor cleanup failure was hidden");
                    const auto retained = store.scan(account(), journal, facts);
                    require(retained.complete() && retained.records.empty() &&
                                retained.preparations.size() == 2,
                            "failed cleanup lost A or its durable successor");
                } else if (point == CrashPoint::successor_cleanup_error) {
                    const auto held = CreateFileW(path_b.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    require(held != INVALID_HANDLE_VALUE, "could not block successor cleanup");
                    const auto status = publisher.publish(b->key, request).status;
                    CloseHandle(held);
                    require(status == ManagedOpeningAllocationPublicationStatus::io_error,
                            "post-publication cleanup failure was hidden");
                    const auto retained = store.scan(account(), journal, facts);
                    require(retained.complete() && retained.records.size() == 1 &&
                                retained.preparations.size() == 2,
                            "allocation plus pending crash state was not retained");
                } else if (point == CrashPoint::pending_successor) {
                    // Simulate interruption after prep-B but before deleting prep-A.
                    const auto both = store.scan(account(), journal, facts);
                    require(both.complete() && both.preparations.size() == 2 && both.records.empty(),
                            "two pending frontiers were not recoverable");
                } else if (point == CrashPoint::canonical_successor_mismatch) {
                    const auto status = publisher.publish(a.record->key, request).status;
                    require(status == ManagedOpeningAllocationPublicationStatus::superseded &&
                                !store.load_preparation(a.record->key).found(),
                            "canonical successor recovery did not remain idempotent");
                } else {
                    const auto status = publisher.publish(a.record->key, request).status;
                    if (point == CrashPoint::successor_write_error) {
                        const auto pending_a = store.load_preparation(a.record->key);
                        const auto pending_b = store.load_preparation(b->key);
                        require((status == ManagedOpeningAllocationPublicationStatus::io_error ||
                                 status == ManagedOpeningAllocationPublicationStatus::invalid_evidence) &&
                                     pending_a.found() && !pending_b.found(),
                                "successor write failure lost predecessor intent");
                        require(std::filesystem::remove(blocked_temporary),
                                "temporary write blocker could not be removed");
                    } else {
                        require(status == ManagedOpeningAllocationPublicationStatus::superseded,
                                "predecessor was not superseded");
                        const auto durable = store.scan(account(), journal, facts);
                        require(durable.complete() &&
                                    durable.records.size() + durable.preparations.size() == 1 &&
                                    !store.load(a.record->key).found() &&
                                    !store.load_preparation(a.record->key).found(),
                                "superseded returned without durable successor intent");
                    }
                }
            } // Drop all source objects as at a process interruption.
            if (point == CrashPoint::canonical_successor_mismatch) {
                std::filesystem::remove_all(root);
                continue;
            }

            WindowsFileJournalStore journal(root / "journal");
            WindowsFileManagedOpeningFactStore facts(root / "facts");
            WindowsSingleWriterLease lease(root / "lease", account());
            WindowsFileManagedOpeningAllocationStore store(root / "allocations");
            ObservationGraph graph(account());
            ObservationBatch current;
            current.account = account();
            current.observed_domains = ObservationDomain::history_deals;
            current.history_deals_window = source_window();
            current.history_deals = {deal(5001, 0.05), deal(5002, 0.03)};
            require(graph.apply(current).accepted(), "restart current history failed");
            ManagedOpeningSourceVerifier verifier(
                journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
            ManagedOpeningAllocationPublisher publisher(verifier, facts, store, journal, lease);
            const auto recovered = store.scan(account(), journal, facts);
            require(recovered.complete() && (!recovered.records.empty() ||
                        !recovered.preparations.empty()), "restart lost all publication intent");
            const auto journal_record = journal.load(operation());
            require(journal_record.found(), "restart journal was not recovered");
            const ManagedOpeningProvenanceRequest request{
                operation(), OperationKind::open, journal_record.record->revision, 88001};
            for (const auto &pending : recovered.preparations) {
                const auto result = publisher.publish(pending.entry.key, request);
                require(result.status == ManagedOpeningAllocationPublicationStatus::superseded ||
                            result.status == ManagedOpeningAllocationPublicationStatus::published ||
                            result.status == ManagedOpeningAllocationPublicationStatus::already_published,
                        "restart could not recover a pending frontier");
            }
            // Superseding A may have created B after the initial scan.
            const auto remaining = store.scan(account(), journal, facts);
            require(remaining.complete(), "successor recovery scan failed");
            for (const auto &pending : remaining.preparations) {
                const auto result = publisher.publish(pending.entry.key, request);
                require(result.status == ManagedOpeningAllocationPublicationStatus::published ||
                            result.status ==
                                ManagedOpeningAllocationPublicationStatus::already_published,
                        "durable successor was not publishable after restart");
            }
            const auto final = store.scan(account(), journal, facts);
            require(final.complete() && final.records.size() == 1 && final.preparations.empty() &&
                        final.records.front().fact_references.size() == 2 &&
                        final.records.front().cumulative_managed_logical_units == 8,
                    "restart did not converge to the exact cumulative successor");
        } catch (...) {
            std::filesystem::remove_all(root);
            throw;
        }
        std::filesystem::remove_all(root);
    }
}

} // namespace

int main() {
    try {
        durable_prepare_publish_restart_and_replay();
        rejects_missing_source_and_stale_lease();
        rejects_stale_source_proof_at_low_level_publication();
        rejects_noncanonical_reserved_paths_and_keeps_pending_separate();
        recovers_through_real_filesystem_sources();
        preserves_historical_frontiers_when_a_partial_fill_arrives();
        preserves_cross_graph_frontier_history();
        recovers_one_durable_successor_after_multiple_supersessions();
        preserves_superseded_publication_intent();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}

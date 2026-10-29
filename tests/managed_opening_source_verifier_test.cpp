/// \file managed_opening_source_verifier_test.cpp
/// \brief Verifies independent durable OPEN source attribution.

#include "managed_opening_source_verifier.hpp"
#include "one_shot_backend.hpp"

#include "support/memory_journal_store.hpp"
#include "support/fake_observation_provider.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
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

class TestAccountProbe final : public runtime::CurrentAccountProbe {
public:
    std::optional<AccountKey> current_account() override { return account(); }
};

class TestTransport final : public runtime::DispatchTransport {
public:
    runtime::BackendCallResult submit_once(const OperationRecord &) override {
        ++calls;
        return {runtime::BackendCallStatus::broker_result,
                runtime::BrokerResultDisposition::reconciling, 10009, {0xA0, 0x01},
                {{70, 5001}}};
    }

    std::size_t calls = 0;
};

Mt5DealSnapshot deal(std::uint64_t ticket, double volume,
                     std::uint64_t order_ticket = 3001,
                     std::uint64_t position_identifier = 88001,
                     std::uint32_t type = 0, std::uint32_t entry = 0,
                     std::int64_t time_msc = 1500) {
    Mt5DealSnapshot value{};
    value.ticket = ticket;
    value.order_ticket = order_ticket;
    value.position_id = position_identifier;
    value.type = type;
    value.entry = entry;
    value.volume = volume;
    value.time_msc = time_msc;
    value.known_fields = kManagedOpeningRequiredDealFields;
    return value;
}

// ObservationGraph is deliberately non-movable. Build fixtures through a
// callback so each test still owns the graph for the verifier's lifetime.
template <typename Callback>
void with_graph(const std::vector<Mt5DealSnapshot> &deals, Callback callback) {
    ObservationGraph graph(account());
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::history_deals;
    baseline_batch.history_deals_window = source_window();
    require(graph.apply(baseline_batch).accepted(),
            "pre-dispatch baseline graph was not accepted");
    const auto baseline = capture_reconciliation_baseline(graph);
    ObservationBatch post_dispatch_batch = baseline_batch;
    post_dispatch_batch.history_deals = deals;
    require(graph.apply(post_dispatch_batch).accepted(),
            "post-dispatch history graph was not accepted");
    callback(graph, baseline);
}

ManagedOpeningDealFact fact_for(const ObservationGraph &graph,
                                std::uint64_t deal_ticket, double volume,
                                std::uint64_t operation_revision = 3) {
    ManagedOpeningDealObservation observation{
        operation(), operation_revision, 3001, deal_ticket, 88001,
        kManagedOpeningRequiredDealFields, BrokerPositionDirection::buy,
        BrokerDealEntry::in, 1500, volume, 0.01,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000},
        graph.domain_revision(ObservationDomain::history_deals),
        graph.instance_id(), graph.domain_revision(ObservationDomain::history_deals),
        source_window().from_msc, source_window().to_msc};
    const ManagedOpeningExecutionRequest request{
        operation(), OperationKind::open, operation_revision, 3001, 88001};
    const auto result = ManagedOpeningDealFact::from_observation(request, observation);
    require(result.has_value(), "valid source fact was rejected");
    return *result;
}

OperationRecord journal_record(const ReconciliationBaseline &baseline,
                               const std::vector<std::uint64_t> &deal_tickets,
                               std::uint64_t operation_revision = 3) {
    std::vector<ReconciliationPredicate> predicates;
    std::vector<ReconciliationBinding> bindings;
    std::uint64_t correlation = 70;
    for (std::size_t index = 0; index != deal_tickets.size(); ++index) {
        const auto ticket = deal_tickets[index];
        if (index == 0) {
            // This is the only result-derived anchor that one validated
            // order_send result can produce. Additional fills are discovered
            // by complete DEAL_ORDER history, not fabricated bindings.
            predicates.push_back(expect_reconciliation_transition(
                ReconciliationPredicateKind::history_deal_present, std::nullopt, false,
                ReconciliationTransition::absent_to_present, correlation,
                source_window()));
            bindings.push_back({correlation++, ticket});
        }
    }
    OperationRecord record;
    record.key = operation();
    record.operation_kind = OperationKind::open;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = OperationState::accepted;
    record.journal_state = JournalState::result_persisted;
    record.revision = operation_revision;
    record.result_source_revision = operation_revision == 1 ? 1 : operation_revision - 1;
    record.fencing_token = 17;
    record.reconciliation_descriptor = ReconciliationDescriptor{
        account(), baseline, std::move(predicates), OperationState::filled,
        operation().trade_id, operation().operation_id,
        deal_tickets.size() == 2 ? 8U : 5U};
    record.reconciliation_bindings = std::move(bindings);
    require(record.valid(), "source journal fixture is invalid");
    return record;
}

void accepts_only_independent_source_chain() {
    with_graph({deal(5001, 0.05), deal(5002, 0.03)},
               [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05), fact_for(graph, 5002, 0.03)};
        mt5bridge_test_support::MemoryJournalStore journal;
        auto record = journal_record(baseline, {5001, 5002});
        require(record.reconciliation_bindings.size() == 1 &&
                    record.reconciliation_descriptor &&
                    record.reconciliation_descriptor->predicates.size() == 1 &&
                    record.reconciliation_descriptor->predicates[0].ticket == 0,
                "multiple-fill fixture fabricated a future ticket");
        journal.records.emplace(operation(), std::move(record));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        const auto result = verifier.verify(
            {operation(), OperationKind::open, 3, 88001});
        require(result.found(), "independent OPEN source chain was not verified");
        require(result.proof->deal_tickets() == std::vector<std::uint64_t>{5001, 5002},
                "source proof did not retain the canonical deal set");
        require(result.proof->managed_logical_units() == 8,
                "source proof cumulative quantity was not exact");
        require(result.proof->evidence_digest() == 14084310942890406028ULL,
                "source fingerprint golden vector changed unexpectedly");
    });
}

void rejects_missing_or_rewritten_sources() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline, {5001}));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "missing durable source fact was accepted");
        facts.facts = {fact_for(graph, 5001, 0.04)};
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "rewritten broker volume was accepted");
    });
}

void rejects_identity_and_history_contradictions() {
    with_graph({deal(5001, 0.05, 3001, 88002)},
               [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline, {5001}));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "foreign position execution was accepted");
        auto request = ManagedOpeningProvenanceRequest{
            operation(), OperationKind::open, 4, 88002};
        require(verifier.verify(request).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "stale journal revision was accepted");
    });
    with_graph({deal(5001, 0.05), deal(5002, 0.01, 3999, 88001)},
               [](const auto &graph, const auto &baseline) {
                   MemoryOpeningFactStore facts;
                   facts.facts = {fact_for(graph, 5001, 0.05)};
                   mt5bridge_test_support::MemoryJournalStore journal;
                   journal.records.emplace(operation(), journal_record(baseline, {5001}));
                   TestLease lease;
                   ManagedOpeningSourceVerifier verifier(
                       journal, facts, graph,
                       ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
                   require(verifier.verify({operation(), OperationKind::open, 3, 88001})
                                   .status ==
                               ManagedOpeningSourceVerificationStatus::invalid_evidence,
                           "external same-position execution was accepted");
               });
    {
        ObservationGraph graph(account());
        ObservationBatch baseline_batch;
        baseline_batch.account = account();
        baseline_batch.observed_domains = ObservationDomain::history_deals;
        baseline_batch.history_deals_window = source_window();
        require(graph.apply(baseline_batch).accepted(),
                "multiple-position baseline fixture was rejected");
        const auto baseline = capture_reconciliation_baseline(graph);
        ObservationBatch batch;
        batch.account = account();
        batch.observed_domains = ObservationDomain::positions |
                                 ObservationDomain::history_deals;
        batch.position_history_order = PositionHistoryOrder::positions_before_history_deals;
        batch.history_deals_window = source_window();
        batch.history_deals = {deal(5001, 0.05)};
        Mt5PositionSnapshot first{};
        first.ticket = 7001;
        first.identifier = 88001;
        first.known_fields = MT5BRIDGE_POSITION_KNOWN_TICKET |
                             MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;
        auto second = first;
        second.ticket = 7002;
        batch.positions = {first, second};
        require(graph.apply(batch).accepted(), "multiple-position fixture was rejected");
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline, {5001}));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "multiple positions for one identifier were accepted");
    }
}

void survives_graph_restart_but_not_lease_revocation() {
    MemoryOpeningFactStore facts;
    mt5bridge_test_support::MemoryJournalStore journal;
    TestLease lease;
    std::vector<ManagedOpeningDealFact> retained;
    with_graph({deal(5001, 0.05)}, [&](const auto &graph, const auto &baseline) {
        retained.push_back(fact_for(graph, 5001, 0.05));
        journal.records.emplace(operation(), journal_record(baseline, {5001}));
    });
    with_graph({deal(5001, 0.05)}, [&](const auto &restarted_graph, const auto &) {
        facts.facts = retained;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, restarted_graph,
            ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        const auto restarted_result =
            verifier.verify({operation(), OperationKind::open, 3, 88001});
        require(restarted_result.found(),
                "durable source proof did not survive graph restart re-observation");
        lease.held = false;
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::lease_not_held,
                "revoked fencing capability was accepted");
    });
}

void accepts_idempotent_repoll_without_rewriting_receipt() {
    ObservationGraph graph(account());
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::history_deals;
    baseline_batch.history_deals_window = source_window();
    require(graph.apply(baseline_batch).accepted(), "repoll baseline was rejected");
    const auto baseline = capture_reconciliation_baseline(graph);
    ObservationBatch post_dispatch = baseline_batch;
    post_dispatch.history_deals = {deal(5001, 0.05)};
    require(graph.apply(post_dispatch).accepted(), "repoll history was rejected");

    MemoryOpeningFactStore facts;
    facts.facts = {fact_for(graph, 5001, 0.05)};
    const auto original_receipt = facts.facts.front().admission_receipt();
    mt5bridge_test_support::MemoryJournalStore journal;
    journal.records.emplace(operation(), journal_record(baseline, {5001}));
    TestLease lease;
    ManagedOpeningSourceVerifier verifier(
        journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    require(verifier.verify({operation(), OperationKind::open, 3, 88001}).found(),
            "first complete poll was not admitted");

    // The same deal observed again gets a newer graph evidence revision. The
    // durable B3A receipt remains the first admission and must not be replaced.
    require(graph.apply(post_dispatch).accepted(), "repoll refresh was rejected");
    require(verifier.verify({operation(), OperationKind::open, 3, 88001}).found(),
            "unchanged complete repoll invalidated source proof");
    require(facts.facts.front().admission_receipt() == original_receipt,
            "repoll changed the immutable admission receipt");
}

void accepts_production_reachable_one_shot_result() {
    using mt5bridge_test_support::FakeObservationProvider;
    ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = ObservationDomain::active_orders |
                                      ObservationDomain::positions |
                                      ObservationDomain::history_orders |
                                      ObservationDomain::history_deals;
    baseline_batch.history_orders_window = source_window();
    baseline_batch.history_deals_window = source_window();
    baseline_batch.position_history_order =
        PositionHistoryOrder::positions_before_history_deals;
    Mt5PositionSnapshot position{};
    position.ticket = 7001;
    position.identifier = 88001;
    position.type = 0;
    position.known_fields = MT5BRIDGE_POSITION_KNOWN_TICKET |
                            MT5BRIDGE_POSITION_KNOWN_IDENTIFIER |
                            MT5BRIDGE_POSITION_KNOWN_TYPE;
    baseline_batch.positions = {position};
    Mt5HistoryOrderSnapshot history_order{};
    history_order.ticket = 3001;
    history_order.position_id = 88001;
    history_order.time_done_msc = 1500;
    history_order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                                 MT5BRIDGE_ORDER_KNOWN_POSITION_ID |
                                 MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
    baseline_batch.history_orders = {history_order};
    ObservationBatch post_dispatch = baseline_batch;
    post_dispatch.history_deals = {deal(5001, 0.05)};
    FakeObservationProvider provider(
        {baseline_batch, baseline_batch, baseline_batch, post_dispatch});
    ObservationCoordinator coordinator(provider, account());
    ObservationCollectionRequest collection;
    collection.history_deals_window = source_window();
    collection.history_orders_window = source_window();
    collection.position_history_order = PositionHistoryOrder::positions_before_history_deals;
    const auto baseline_sample = coordinator.refresh(collection);
    require(baseline_sample.sample.has_value(), "production baseline observation failed");
    const auto first = coordinator.refresh(collection);
    const auto second = coordinator.refresh(collection);
    require(first.sample && second.sample, "production history observations failed");
    EnvironmentConsistencyRequest consistency_request;
    consistency_request.history_deals_window = source_window();
    consistency_request.history_orders_window = source_window();
    const auto consistency = EnvironmentConsistencyPolicy::evaluate(
        {*first.sample, *second.sample}, consistency_request);
    require(consistency.consistent() && consistency.proof,
            "production consistency proof was not admitted");
    const auto baseline = coordinator.capture_baseline();

    const auto descriptor = ReconciliationDescriptor{
        account(), baseline,
        {expect_reconciliation_transition(
            ReconciliationPredicateKind::history_deal_present, std::nullopt, false,
            ReconciliationTransition::absent_to_present, 70, source_window())},
        OperationState::filled, operation().trade_id, operation().operation_id, 5};
    mt5bridge_test_support::MemoryJournalStore journal_store;
    OperationJournal journal(journal_store);
    require(journal.create(operation(), {0x01}, OperationKind::open).accepted(),
            "production journal create failed");
    require(journal.transition_operation(operation(), OperationState::prechecking).accepted(),
            "production prechecking failed");
    require(journal.transition_journal(operation(), JournalState::prechecked).accepted(),
            "production prechecked failed");
    require(journal.transition_journal(operation(), JournalState::dispatch_intent_persisted)
                .accepted(),
            "production dispatch-intent transition failed");
    require(journal.persist_reconciliation_descriptor(operation(), descriptor).accepted(),
            "production descriptor persistence failed");
    TestLease lease;
    DispatchAdmissionBarrier admission(journal, coordinator.graph(),
                                       EnvironmentConsistencyRequest{});
    DispatchAdmissionRequest admission_request;
    admission_request.current_account = account();
    admission_request.environment_proof = consistency.proof;
    auto admitted = admission.admit(operation(), admission_request, lease);
    require(admitted.admitted(), "production dispatch barrier did not admit");
    runtime::OneShotDispatchBackend backend;
    TestTransport transport;
    TestAccountProbe account_probe;
    const auto executed = backend.execute(journal, operation(), std::move(*admitted.permit),
                                          account_probe, lease, transport);
    require(executed.completed() && executed.record && transport.calls == 1,
            "production one-shot dispatch did not complete");
    require(executed.record->journal_state == JournalState::result_persisted &&
                executed.record->operation_state == OperationState::reconciling &&
                executed.record->result_source_revision != 0,
                "result-source anchor was not retained across lifecycle transition");

    const auto post_dispatch_sample = coordinator.refresh(collection);
    require(post_dispatch_sample.sample.has_value(),
            "production post-dispatch history observation failed");

    MemoryOpeningFactStore facts;
    facts.facts = {fact_for(coordinator.graph(), 5001, 0.05, executed.record->revision)};
    ManagedOpeningSourceVerifier verifier(
        journal_store, facts, coordinator.graph(),
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    require(verifier.verify({operation(), OperationKind::open, executed.record->revision, 88001})
                .found(),
            "production-reachable result did not pass source verifier");
}

void rejects_invalid_requests_and_store_failures() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(baseline, {5001}));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        auto invalid = ManagedOpeningProvenanceRequest{operation(), OperationKind::close, 3,
                                                       88001};
        require(verifier.verify(invalid).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_request,
                "non-OPEN query was accepted");
        facts.scan_status = ManagedOpeningFactScanStatus::io_error;
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::io_error,
                "fact-store I/O failure was hidden");
    });
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        auto record = journal_record(baseline, {5001});
        record.reconciliation_descriptor->predicates.front().ticket = 5001;
        record.reconciliation_bindings.clear();
        require(record.valid(), "known-ticket journal fixture is invalid");
        journal.records.emplace(operation(), std::move(record));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "caller-known history ticket was treated as result authority");
    });
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        auto record = journal_record(baseline, {5001});
        record.reconciliation_descriptor->predicates.push_back(
            expect_reconciliation_transition(
                ReconciliationPredicateKind::history_deal_present, 5002, false,
                ReconciliationTransition::absent_to_present, 0, source_window()));
        require(record.valid(), "fabricated future-ticket fixture is invalid");
        journal.records.emplace(operation(), std::move(record));
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "fabricated future history ticket was accepted");
    });
}

void rejects_non_causal_or_stale_history_evidence() {
    // The deal was already present in the graph when the durable baseline was
    // captured. A later verifier call cannot turn that into absent_to_present.
    ObservationGraph preexisting_graph(account());
    ObservationBatch existing;
    existing.account = account();
    existing.observed_domains = ObservationDomain::history_deals;
    existing.history_deals_window = source_window();
    existing.history_deals = {deal(5001, 0.05)};
    require(preexisting_graph.apply(existing).accepted(),
            "preexisting-deal fixture was rejected");
    const auto baseline = capture_reconciliation_baseline(preexisting_graph);
    MemoryOpeningFactStore facts;
    facts.facts = {fact_for(preexisting_graph, 5001, 0.05)};
    mt5bridge_test_support::MemoryJournalStore journal;
    journal.records.emplace(operation(), journal_record(baseline, {5001}));
    TestLease lease;
    ManagedOpeningSourceVerifier verifier(
        journal, facts, preexisting_graph,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                ManagedOpeningSourceVerificationStatus::invalid_evidence,
            "deal existing before baseline was accepted");
    require(preexisting_graph.apply(existing).accepted(),
            "preexisting-deal repoll was rejected");
    facts.facts = {fact_for(preexisting_graph, 5001, 0.05)};
    require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                ManagedOpeningSourceVerificationStatus::invalid_evidence,
            "repoll of a preexisting deal was accepted as a new admission");

    // A later deal batch without a complete window must not inherit an old
    // complete-window coverage entry.
    ObservationGraph stale_coverage_graph(account());
    require(stale_coverage_graph.apply(existing).accepted(),
            "stale coverage baseline was rejected");
    const auto stale_baseline = capture_reconciliation_baseline(stale_coverage_graph);
    ObservationBatch uncovered = existing;
    uncovered.history_deals = {deal(5001, 0.05)};
    uncovered.history_deals_window.reset();
    require(stale_coverage_graph.apply(uncovered).accepted(),
            "uncovered deal batch was rejected");
    MemoryOpeningFactStore stale_facts;
    stale_facts.facts = {fact_for(stale_coverage_graph, 5001, 0.05)};
    mt5bridge_test_support::MemoryJournalStore stale_journal;
    stale_journal.records.emplace(operation(), journal_record(stale_baseline, {5001}));
    ManagedOpeningSourceVerifier stale_verifier(
        stale_journal, stale_facts, stale_coverage_graph,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
    require(stale_verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                ManagedOpeningSourceVerificationStatus::invalid_evidence,
            "old complete coverage authorized an uncovered deal");
}

void rejects_unreconstructable_lifecycle_and_volume_states() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph, const auto &baseline) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        auto record = journal_record(baseline, {5001});
        record.reconciliation_descriptor->requested_volume = 4;
        require(record.valid(), "under-requested volume fixture is invalid");
        journal.records.emplace(operation(), record);
        TestLease lease;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, graph, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "managed volume above requested quantity was accepted");

        record.reconciliation_descriptor->requested_volume = 5;
        record.journal_state = JournalState::reconciling;
        record.operation_state = OperationState::reconciling;
        record.revision = 4;
        require(record.valid(), "lifecycle revision fixture is invalid");
        journal.records[operation()] = record;
        require(verifier.verify({operation(), OperationKind::open, 4, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::invalid_evidence,
                "latest lifecycle revision was treated as immutable source anchor");
    });
}

#if defined(_WIN32)
void survives_filesystem_store_reload() {
    const auto root = std::filesystem::temp_directory_path() /
                      "mt5bridge-b3b-source-reload";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    try {
        std::vector<ManagedOpeningDealFact> retained;
        with_graph({deal(5001, 0.05)}, [&](const auto &graph, const auto &baseline) {
            retained.push_back(fact_for(graph, 5001, 0.05, 1));
            const auto record = journal_record(baseline, {5001}, 1);
            WindowsFileJournalStore journal_store(root / "journal");
            WindowsFileManagedOpeningFactStore fact_store(root / "facts");
            require(journal_store.ready() && fact_store.ready(),
                    "filesystem source stores did not open");
            WindowsSingleWriterLease lease(root / "journal", account());
            require(lease.ready(), "filesystem source lease did not open");
            require(fact_store.commit(retained.front(), lease) ==
                        ManagedOpeningFactCommitStatus::committed,
                    "durable source fact was not committed");
            const auto journal_status = journal_store.commit(record, std::nullopt);
            if (journal_status != StoreCommitStatus::committed)
                std::cerr << "journal reload fixture commit failed: "
                          << journal_store.last_error() << '\n';
            require(journal_status == StoreCommitStatus::committed,
                    "durable source journal was not committed");
        });

        with_graph({deal(5001, 0.05)}, [&](const auto &recovered_graph, const auto &) {
            WindowsFileJournalStore journal_store(root / "journal");
            WindowsFileManagedOpeningFactStore fact_store(root / "facts");
            WindowsSingleWriterLease lease(root / "journal", account());
            require(journal_store.ready() && fact_store.ready() && lease.ready(),
                    "restarted filesystem source stores did not open");
            ManagedOpeningSourceVerifier verifier(
                journal_store, fact_store, recovered_graph,
                ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
            require(verifier.verify({operation(), OperationKind::open, 1, 88001}).found(),
                    "durable source chain was not recovered after store restart");
        });
    } catch (...) {
        std::filesystem::remove_all(root, cleanup_error);
        throw;
    }
    std::filesystem::remove_all(root, cleanup_error);
}
#endif

} // namespace

int main() {
    try {
        accepts_only_independent_source_chain();
        rejects_missing_or_rewritten_sources();
        rejects_identity_and_history_contradictions();
        survives_graph_restart_but_not_lease_revocation();
        accepts_idempotent_repoll_without_rewriting_receipt();
        accepts_production_reachable_one_shot_result();
        rejects_invalid_requests_and_store_failures();
        rejects_non_causal_or_stale_history_evidence();
        rejects_unreconstructable_lifecycle_and_volume_states();
#if defined(_WIN32)
        survives_filesystem_store_reload();
#endif
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

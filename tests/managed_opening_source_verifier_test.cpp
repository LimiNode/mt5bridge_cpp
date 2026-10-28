/// \file managed_opening_source_verifier_test.cpp
/// \brief Verifies independent durable OPEN source attribution.

#include "managed_opening_source_verifier.hpp"

#include "support/memory_journal_store.hpp"

#include <algorithm>
#include <cstdint>
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
    ObservationBatch batch;
    batch.account = account();
    batch.observed_domains = ObservationDomain::history_deals;
    batch.history_deals_window = source_window();
    batch.history_deals = deals;
    require(graph.apply(batch).accepted(), "history source graph was not accepted");
    callback(graph);
}

ManagedOpeningDealFact fact_for(const ObservationGraph &graph,
                                std::uint64_t deal_ticket, double volume) {
    ManagedOpeningDealObservation observation{
        operation(), 3, 3001, deal_ticket, 88001,
        kManagedOpeningRequiredDealFields, BrokerPositionDirection::buy,
        BrokerDealEntry::in, 1500, volume, 0.01,
        ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000},
        graph.domain_revision(ObservationDomain::history_deals),
        graph.instance_id(), graph.domain_revision(ObservationDomain::history_deals),
        source_window().from_msc, source_window().to_msc};
    const ManagedOpeningExecutionRequest request{
        operation(), OperationKind::open, 3, 3001, 88001};
    const auto result = ManagedOpeningDealFact::from_observation(request, observation);
    require(result.has_value(), "valid source fact was rejected");
    return *result;
}

OperationRecord journal_record(const ObservationGraph &graph,
                               const std::vector<std::uint64_t> &deal_tickets) {
    std::vector<ReconciliationPredicate> predicates;
    std::vector<ReconciliationBinding> bindings;
    std::uint64_t correlation = 70;
    for (const auto ticket : deal_tickets) {
        predicates.push_back(expect_reconciliation_transition(
            ReconciliationPredicateKind::history_deal_present, std::nullopt, false,
            ReconciliationTransition::absent_to_present, correlation,
            source_window()));
        bindings.push_back({correlation++, ticket});
    }
    OperationRecord record;
    record.key = operation();
    record.operation_kind = OperationKind::open;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = OperationState::accepted;
    record.journal_state = JournalState::result_persisted;
    record.revision = 3;
    record.fencing_token = 17;
    record.reconciliation_descriptor = ReconciliationDescriptor{
        account(), capture_reconciliation_baseline(graph), std::move(predicates),
        OperationState::filled, operation().trade_id, operation().operation_id, 0};
    record.reconciliation_bindings = std::move(bindings);
    require(record.valid(), "source journal fixture is invalid");
    return record;
}

void accepts_only_independent_source_chain() {
    with_graph({deal(5001, 0.05), deal(5002, 0.03)}, [](const auto &graph) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05), fact_for(graph, 5002, 0.03)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(graph, {5001, 5002}));
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
        require(result.proof->evidence_digest() != 0,
                "source proof digest was empty");
    });
}

void rejects_missing_or_rewritten_sources() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph) {
        MemoryOpeningFactStore facts;
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(graph, {5001}));
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
    with_graph({deal(5001, 0.05, 3001, 88002)}, [](const auto &graph) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(graph, {5001}));
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
               [](const auto &graph) {
                   MemoryOpeningFactStore facts;
                   facts.facts = {fact_for(graph, 5001, 0.05)};
                   mt5bridge_test_support::MemoryJournalStore journal;
                   journal.records.emplace(operation(), journal_record(graph, {5001}));
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
        journal.records.emplace(operation(), journal_record(graph, {5001}));
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
    with_graph({deal(5001, 0.05)}, [&](const auto &graph) {
        retained.push_back(fact_for(graph, 5001, 0.05));
        journal.records.emplace(operation(), journal_record(graph, {5001}));
    });
    with_graph({deal(5001, 0.05)}, [&](const auto &restarted_graph) {
        facts.facts = retained;
        ManagedOpeningSourceVerifier verifier(
            journal, facts, restarted_graph,
            ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000}, lease);
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).found(),
                "source proof did not survive graph restart");
        lease.held = false;
        require(verifier.verify({operation(), OperationKind::open, 3, 88001}).status ==
                    ManagedOpeningSourceVerificationStatus::lease_not_held,
                "revoked fencing capability was accepted");
    });
}

void rejects_invalid_requests_and_store_failures() {
    with_graph({deal(5001, 0.05)}, [](const auto &graph) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        journal.records.emplace(operation(), journal_record(graph, {5001}));
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
    with_graph({deal(5001, 0.05)}, [](const auto &graph) {
        MemoryOpeningFactStore facts;
        facts.facts = {fact_for(graph, 5001, 0.05)};
        mt5bridge_test_support::MemoryJournalStore journal;
        auto record = journal_record(graph, {5001});
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
}

} // namespace

int main() {
    try {
        accepts_only_independent_source_chain();
        rejects_missing_or_rewritten_sources();
        rejects_identity_and_history_contradictions();
        survives_graph_restart_but_not_lease_revocation();
        rejects_invalid_requests_and_store_failures();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

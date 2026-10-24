/// \file managed_inout_close_delta_proof_test.cpp
/// \brief Exercises the private authoritative INOUT close proof producer.

#include "managed_inout_close_delta_proof.hpp"

#include <mt5bridge/reconciliation/engine.hpp>

#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

Mt5DealSnapshot inout_deal(std::uint64_t ticket = 701) {
    Mt5DealSnapshot result{};
    result.ticket = ticket;
    result.order_ticket = 702;
    result.position_id = 800;
    result.type = 1;
    result.entry = static_cast<std::uint32_t>(mt5bridge::BrokerDealEntry::inout);
    result.volume = 6.0;
    result.time_msc = 1500;
    std::strcpy(result.symbol, "EURUSD");
    result.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                          MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                          MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                          MT5BRIDGE_DEAL_KNOWN_TYPE |
                          MT5BRIDGE_DEAL_KNOWN_ENTRY |
                          MT5BRIDGE_DEAL_KNOWN_VOLUME |
                          MT5BRIDGE_DEAL_KNOWN_TIME |
                          MT5BRIDGE_DEAL_KNOWN_SYMBOL;
    return result;
}

mt5bridge::OperationRecord operation(
    const mt5bridge::ReconciliationBaseline &baseline, std::uint64_t graph_instance_id,
    std::uint64_t settled_volume = 2) {
    const mt5bridge::OperationKey key{account(), 71, 2};
    mt5bridge::ReconciliationDescriptor descriptor{
        key.account, baseline,
        {mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::history_deal_present, 701, false,
            mt5bridge::ReconciliationTransition::absent_to_present, 0,
            mt5bridge::ObservationWindow{1000, 2000})},
        settled_volume == 3 ? mt5bridge::OperationState::filled
                            : mt5bridge::OperationState::partially_filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = 3;

    mt5bridge::OperationRecord result;
    result.key = key;
    result.operation_kind = mt5bridge::OperationKind::close;
    result.settlement_mode = mt5bridge::ManagedSettlementMode::allocation_gated;
    result.request_payload = {0x01};
    result.result_payload = {0x02};
    result.operation_state = descriptor.settled_state;
    result.journal_state = mt5bridge::JournalState::reconciling;
    result.revision = 8;
    result.fencing_token = graph_instance_id;
    result.reconciliation_descriptor = std::move(descriptor);
    result.settled_volume = settled_volume;
    require(result.valid(), "INOUT operation fixture is invalid");
    return result;
}

mt5bridge::BrokerReversalRecord reversal(std::uint64_t graph_instance_id) {
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
    result.provenance = {graph_instance_id, 1, 2, 1, 2, 2, 202};
    require(result.valid(), "INOUT reversal fixture is invalid");
    return result;
}

void build_states(mt5bridge::managed_trade::ManagedTradeState *pre,
                  mt5bridge::managed_trade::ManagedTradeState *post) {
    require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                pre, mt5bridge::managed_trade::TradeId{71}, 10, 10, 4),
            "pre state initialization failed");
    require(pre->start_open_slice(10) ==
                mt5bridge::managed_trade::MutationStatus::applied &&
                pre->enter_submitting() ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->record_fill(10) == mt5bridge::managed_trade::MutationStatus::applied &&
                pre->reconcile() == mt5bridge::managed_trade::MutationStatus::applied &&
                pre->request_close() ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->start_close(3) ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->enter_submitting() ==
                    mt5bridge::managed_trade::MutationStatus::applied,
            "pre state close setup failed");
    *post = *pre;
    require(post->record_fill(2) == mt5bridge::managed_trade::MutationStatus::applied &&
                post->reconcile() == mt5bridge::managed_trade::MutationStatus::applied,
            "post state close setup failed");
}

struct Fixture {
    mt5bridge::ObservationGraph graph{account()};
    std::optional<mt5bridge::ReconciliationBaseline> baseline;
    mt5bridge::OperationRecord operation_record;
    mt5bridge::BrokerReversalRecord reversal_record;
    mt5bridge::ManagedOwnershipBasis basis_record;
    mt5bridge::managed_trade::ManagedTradeState pre_state;
    mt5bridge::managed_trade::ManagedTradeState post_state;

    Fixture() {
        mt5bridge::ObservationBatch baseline_batch;
        baseline_batch.account = account();
        baseline_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        baseline_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        require(graph.apply(baseline_batch).accepted(), "proof baseline was not accepted");
        baseline = mt5bridge::capture_reconciliation_baseline(graph);

        mt5bridge::ObservationBatch deal_batch;
        deal_batch.account = account();
        deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        deal_batch.history_deals = {inout_deal()};
        require(graph.apply(deal_batch).accepted(), "proof deal observation was not accepted");

        operation_record = operation(*baseline, graph.instance_id());
        reversal_record = reversal(graph.instance_id());
        basis_record.account = account();
        basis_record.basis_key = {reversal_record.key(), operation_record.key};
        basis_record.broker_provenance = reversal_record.provenance;
        basis_record.operation_revision = operation_record.revision;
        require(basis_record.valid(), "proof ownership basis fixture is invalid");
        build_states(&pre_state, &post_state);
    }
};

void accepts_authoritative_inout_delta() {
    Fixture fixture;
    require(fixture.graph.domain_revision(mt5bridge::ObservationDomain::history_deals) == 2,
            "proof fixture history revision is unexpected");
    require(fixture.graph.history_deals_covered(mt5bridge::ObservationWindow{1000, 2000},
                                               fixture.baseline->history_deals_revision()),
            "proof fixture history coverage is missing");
    require(fixture.graph.history_deal_evidence_revision(701) == 2,
            "proof fixture deal evidence revision is unexpected");
    require(fixture.pre_state.valid() && fixture.post_state.valid(),
            "proof fixture managed states are invalid");
    require(fixture.operation_record.settlement_mode ==
                mt5bridge::ManagedSettlementMode::allocation_gated &&
                mt5bridge::journal_at_least_result_persisted(
                    fixture.operation_record.journal_state) &&
                fixture.operation_record.operation_state ==
                    mt5bridge::OperationState::partially_filled,
            "proof fixture operation gate is unexpected");
    require(mt5bridge::dispatch::detail::inout_proof_history_predicate(
                 fixture.operation_record, fixture.reversal_record.deal_ticket)
                 .has_value(),
            "proof fixture history predicate is not bound");
    require(fixture.reversal_record.provenance.graph_instance_id ==
                fixture.graph.instance_id() &&
                fixture.reversal_record.provenance.post_graph_revision == 2 &&
                fixture.reversal_record.provenance.history_deals_revision == 2,
            "proof fixture reversal provenance is unexpected");
    const auto deal = mt5bridge::dispatch::detail::inout_proof_deal(
        fixture.graph, fixture.reversal_record.deal_ticket);
    require(deal.has_value(), "proof fixture deal lookup failed");
    require(deal->order_ticket == fixture.reversal_record.order_ticket &&
                deal->position_id == fixture.reversal_record.deal_position_id &&
                deal->entry ==
                    static_cast<std::uint32_t>(mt5bridge::BrokerDealEntry::inout),
            "proof fixture deal identity mismatch");
    const auto pre_frontier =
        mt5bridge::dispatch::detail::inout_proof_frontier(fixture.pre_state);
    const auto post_frontier =
        mt5bridge::dispatch::detail::inout_proof_frontier(fixture.post_state);
    require(pre_frontier.trade_id == fixture.operation_record.key.trade_id &&
                post_frontier.trade_id == fixture.operation_record.key.trade_id &&
                pre_frontier.operation_id == fixture.operation_record.key.operation_id &&
                post_frontier.operation_id == fixture.operation_record.key.operation_id,
            "proof fixture state identity mismatch");
    const auto proof = mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
        fixture.operation_record, fixture.basis_record, fixture.reversal_record,
        *fixture.baseline, fixture.graph, fixture.pre_state, fixture.post_state);
    require(proof.has_value() && proof->valid(),
            "authoritative INOUT close delta was not produced");
    require(proof->deal_ticket() == 701 && proof->managed_logical_delta() == 2 &&
                proof->managed_close_intent_volume() == 3 &&
                proof->pre_exposure().open_volume == 10 &&
                proof->post_exposure().open_volume == 8 &&
                proof->observation_provenance().graph_instance_id ==
                    fixture.graph.instance_id(),
            "INOUT proof did not retain exact managed binding and frontier");
}

void rejects_arbitrary_delta_or_ambiguous_state() {
    Fixture fixture;
    auto forged = fixture.post_state;
    forged.open_volume = 7;
    const auto forged_result =
        mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
            fixture.operation_record, fixture.basis_record, fixture.reversal_record,
            *fixture.baseline, fixture.graph, fixture.pre_state, forged);
    require(!forged_result.has_value(),
            "caller-forged exposure delta was accepted as authoritative");

    auto foreign_reversal = fixture.reversal_record;
    foreign_reversal.deal_ticket = 702;
    const auto foreign_result =
        mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
            fixture.operation_record, fixture.basis_record, foreign_reversal,
            *fixture.baseline, fixture.graph, fixture.pre_state, fixture.post_state);
    require(!foreign_result.has_value(), "foreign broker deal was accepted by proof producer");

    auto foreign_volume = fixture.reversal_record;
    foreign_volume.pre_volume.units = 600;
    foreign_volume.broker_close_leg = foreign_volume.pre_volume;
    foreign_volume.deal_volume.units = 700;
    require(foreign_volume.valid(), "foreign volume reversal fixture is invalid");
    const auto foreign_volume_result =
        mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
            fixture.operation_record, fixture.basis_record, foreign_volume,
            *fixture.baseline, fixture.graph, fixture.pre_state, fixture.post_state);
    require(!foreign_volume_result.has_value(),
            "foreign broker volume was accepted by proof producer");
}

void rejects_stale_or_ambiguous_history() {
    Fixture stale;
    mt5bridge::ObservationBatch unrelated_batch;
    unrelated_batch.account = account();
    unrelated_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    unrelated_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    require(stale.graph.apply(unrelated_batch).accepted(),
            "stale-history batch was not accepted");
    require(!mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
                  stale.operation_record, stale.basis_record, stale.reversal_record,
                  *stale.baseline, stale.graph, stale.pre_state, stale.post_state)
                  .has_value(),
            "stale INOUT history evidence was accepted");

    Fixture ambiguous;
    auto second = inout_deal(702);
    second.order_ticket = 702;
    second.position_id = 800;
    mt5bridge::ObservationBatch ambiguous_batch;
    ambiguous_batch.account = account();
    ambiguous_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    ambiguous_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    ambiguous_batch.history_deals = {inout_deal(), second};
    require(ambiguous.graph.apply(ambiguous_batch).accepted(),
            "ambiguous-history batch was not accepted");
    require(!mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
                  ambiguous.operation_record, ambiguous.basis_record,
                  ambiguous.reversal_record, *ambiguous.baseline, ambiguous.graph,
                  ambiguous.pre_state, ambiguous.post_state)
                  .has_value(),
            "multiple fresh INOUT deals were accepted as one contribution");
}

void rejects_non_inout_reversal() {
    Fixture fixture;
    auto non_inout = fixture.reversal_record;
    non_inout.deal_entry = mt5bridge::BrokerDealEntry::out;
    require(!non_inout.valid(), "non-INOUT reversal fixture unexpectedly valid");
    require(!mt5bridge::dispatch::ManagedInoutCloseDeltaProofProducer::create(
                  fixture.operation_record, fixture.basis_record, non_inout,
                  *fixture.baseline, fixture.graph, fixture.pre_state, fixture.post_state)
                  .has_value(),
            "non-INOUT broker evidence bypassed the proof boundary");
}

} // namespace

int main() {
    try {
        accepts_authoritative_inout_delta();
        rejects_arbitrary_delta_or_ambiguous_state();
        rejects_stale_or_ambiguous_history();
        rejects_non_inout_reversal();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "managed INOUT close delta proof tests passed\n";
    return 0;
}

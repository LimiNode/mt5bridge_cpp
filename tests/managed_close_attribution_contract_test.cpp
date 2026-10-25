/// \file managed_close_attribution_contract_test.cpp
/// \brief Exercises the fail-closed C3 managed-attribution prerequisite.

#include "managed_close_attribution_contract.hpp"

#include <mt5bridge/reconciliation/engine.hpp>

#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

Mt5DealSnapshot inout_deal() {
    Mt5DealSnapshot result{};
    result.ticket = 701;
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
    require(result.valid(), "reversal fixture is invalid");
    return result;
}

mt5bridge::OperationRecord operation(
    const mt5bridge::ReconciliationBaseline &baseline,
    std::uint64_t graph_instance_id, std::uint64_t close_units) {
    const mt5bridge::OperationKey key{account(), 71, 2};
    mt5bridge::ReconciliationDescriptor descriptor{
        key.account, baseline,
        {mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::history_deal_present, 0, false,
            mt5bridge::ReconciliationTransition::absent_to_present, 9001,
            mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = close_units;

    mt5bridge::OperationRecord result;
    result.key = key;
    result.operation_kind = mt5bridge::OperationKind::close;
    result.settlement_mode = mt5bridge::ManagedSettlementMode::allocation_gated;
    result.request_payload = {0x01};
    result.result_payload = {0x02};
    result.operation_state = mt5bridge::OperationState::filled;
    result.journal_state = mt5bridge::JournalState::reconciling;
    result.revision = 8;
    result.fencing_token = graph_instance_id;
    result.reconciliation_descriptor = std::move(descriptor);
    result.reconciliation_bindings = {{9001, 701}};
    result.settled_volume = close_units;
    require(result.valid(), "operation fixture is invalid");
    return result;
}

mt5bridge::ManagedStagedOwnershipAssociation staged_ownership(
    const mt5bridge::OperationRecord &operation_record,
    const mt5bridge::BrokerReversalRecord &reversal_record) {
    mt5bridge::ManagedStagedOwnershipAssociation result{
        account(),
        {reversal_record.key(), operation_record.key},
        reversal_record.provenance,
        operation_record.revision - 1,
        operation_record.revision,
        mt5bridge::dispatch::detail::inout_candidate_close_intent_digest(
            operation_record, reversal_record.deal_ticket)};
    require(result.valid(), "staged ownership fixture is invalid");
    return result;
}

void build_states(std::uint64_t close_units,
                  mt5bridge::managed_trade::ManagedTradeState *pre,
                  mt5bridge::managed_trade::ManagedTradeState *post) {
    require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                pre, mt5bridge::managed_trade::TradeId{71}, 5, 5, 4),
            "managed state initialization failed");
    require(pre->start_open_slice(5) ==
                mt5bridge::managed_trade::MutationStatus::applied &&
                pre->enter_submitting() ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->record_fill(5) ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->reconcile() ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->request_close() ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->start_close(close_units) ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                pre->enter_submitting() ==
                    mt5bridge::managed_trade::MutationStatus::applied,
            "managed pre-state setup failed");
    *post = *pre;
    require(post->record_fill(close_units) ==
                mt5bridge::managed_trade::MutationStatus::applied &&
                post->reconcile() ==
                    mt5bridge::managed_trade::MutationStatus::applied,
            "managed post-state setup failed");
}

class MemoryOwnershipSource final
    : public mt5bridge::dispatch::DurableManagedPositionOwnershipSource {
public:
    mt5bridge::dispatch::ManagedPositionOwnershipLoadStatus status =
        mt5bridge::dispatch::ManagedPositionOwnershipLoadStatus::found;
    std::optional<mt5bridge::dispatch::ManagedPositionOwnershipFrontier> record;

    mt5bridge::dispatch::ManagedPositionOwnershipLoadResult load(
        const mt5bridge::dispatch::ManagedPositionOwnershipFrontierKey &) const override {
        return {status, record};
    }
};

struct Fixture {
    mt5bridge::ObservationGraph graph{account()};
    std::optional<mt5bridge::ReconciliationBaseline> baseline;
    mt5bridge::BrokerReversalRecord reversal_record;

    Fixture() {
        mt5bridge::ObservationBatch baseline_batch;
        baseline_batch.account = account();
        baseline_batch.observed_domains =
            mt5bridge::ObservationDomain::history_deals;
        baseline_batch.history_deals_window =
            mt5bridge::ObservationWindow{1000, 2000};
        require(graph.apply(baseline_batch).accepted(),
                "baseline observation was not accepted");
        baseline = mt5bridge::capture_reconciliation_baseline(graph);

        mt5bridge::ObservationBatch deal_batch;
        deal_batch.account = account();
        deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deal_batch.history_deals_window =
            mt5bridge::ObservationWindow{1000, 2000};
        deal_batch.history_deals = {inout_deal()};
        require(graph.apply(deal_batch).accepted(),
                "deal observation was not accepted");
        reversal_record = reversal(graph.instance_id());
    }

    std::optional<mt5bridge::dispatch::ManagedInoutCloseDeltaCandidate>
    candidate(std::uint64_t close_units = 5) const {
        const auto operation_record =
            operation(*baseline, graph.instance_id(), close_units);
        const auto staged = staged_ownership(operation_record, reversal_record);
        mt5bridge::managed_trade::ManagedTradeState pre;
        mt5bridge::managed_trade::ManagedTradeState post;
        build_states(close_units, &pre, &post);
        return mt5bridge::dispatch::ManagedInoutCloseDeltaCandidateProducer::create(
            operation_record, staged, reversal_record, *baseline, graph, pre, post);
    }

    mt5bridge::dispatch::ManagedPositionOwnershipFrontier frontier() const {
        mt5bridge::dispatch::ManagedPositionOwnershipFrontier result;
        result.key = {account(), reversal_record.pre_position_identifier,
                      reversal_record.provenance.graph_instance_id,
                      reversal_record.provenance.pre_graph_revision,
                      reversal_record.provenance.pre_positions_revision};
        result.symbol = reversal_record.symbol;
        result.direction = reversal_record.pre_direction;
        result.position_volume = reversal_record.pre_volume;
        result.source_evidence_digest = 3001;
        result.entries = {{
            mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
            71,
            5,
            {500, 2, 1},
            mt5bridge::OperationKey{account(), 71, 1},
            4,
            4001,
        }};
        require(result.valid(), "ownership frontier fixture is invalid");
        return result;
    }
};

void authorizes_exact_exclusive_full_close() {
    Fixture fixture;
    const auto candidate = fixture.candidate();
    require(candidate && candidate->valid(), "full-close candidate was not produced");
    require(candidate->post_exposure().close_satisfied,
            "full-close fixture did not satisfy its close obligation");
    MemoryOwnershipSource source;
    source.record = fixture.frontier();

    const auto result =
        mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
            *candidate, fixture.reversal_record, source);
    require(result.authorized(), "exclusive full close was not authorized");
    require(result.authorization->operation_key() == candidate->operation_key() &&
                result.authorization->deal_ticket() == candidate->deal_ticket() &&
                result.authorization->managed_logical_delta() == 5 &&
                result.authorization->broker_close_leg() ==
                    fixture.reversal_record.broker_close_leg &&
                result.authorization->candidate_digest() ==
                    candidate->evidence_digest() &&
                result.authorization->frontier_digest() ==
                    mt5bridge::dispatch::managed_position_ownership_frontier_digest(
                        *source.record),
            "authorization did not retain exact source identities");
}

void rejects_missing_invalid_or_stale_frontier() {
    Fixture fixture;
    const auto candidate = fixture.candidate();
    require(candidate.has_value(), "candidate setup failed");

    MemoryOwnershipSource missing;
    missing.status =
        mt5bridge::dispatch::ManagedPositionOwnershipLoadStatus::not_found;
    auto result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, missing);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::missing_frontier &&
                !result.authorization,
            "missing ownership frontier did not fail closed");

    MemoryOwnershipSource invalid;
    invalid.record = fixture.frontier();
    invalid.record->entries.front().broker_volume.units = 400;
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, invalid);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::invalid_frontier &&
                !result.authorization,
            "incomplete ownership frontier did not fail closed");

    MemoryOwnershipSource stale;
    stale.record = fixture.frontier();
    stale.record->symbol = "GBPUSD";
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, stale);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::stale_frontier &&
                !result.authorization,
            "stale pre-position identity was accepted");

    MemoryOwnershipSource stale_revision;
    stale_revision.record = fixture.frontier();
    ++stale_revision.record->key.graph_revision;
    require(stale_revision.record->valid(), "stale-revision fixture is invalid");
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, stale_revision);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::stale_frontier &&
                !result.authorization,
            "stale ownership revision was accepted");

    MemoryOwnershipSource failed;
    failed.status = mt5bridge::dispatch::ManagedPositionOwnershipLoadStatus::io_error;
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, failed);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::io_error &&
                !result.authorization,
            "ownership-source I/O failure did not fail closed");
}

void rejects_competing_managed_or_unmanaged_ownership() {
    Fixture fixture;
    const auto candidate = fixture.candidate();
    require(candidate.has_value(), "candidate setup failed");

    MemoryOwnershipSource competing;
    competing.record = fixture.frontier();
    competing.record->entries = {
        {mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
         71, 3, {300, 2, 1}, mt5bridge::OperationKey{account(), 71, 1}, 4, 4001},
        {mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
         72, 2, {200, 2, 1}, mt5bridge::OperationKey{account(), 72, 1}, 4, 4002},
    };
    require(competing.record->valid(), "competing-owner fixture is invalid");
    auto result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, competing);
    require(result.status == mt5bridge::dispatch::
                                 ManagedInoutCloseAttributionStatus::
                                     ambiguous_managed_ownership &&
                !result.authorization,
            "competing managed owner was attributed to the target trade");

    MemoryOwnershipSource external;
    external.record = fixture.frontier();
    external.record->entries = {
        {mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
         71, 4, {400, 2, 1}, mt5bridge::OperationKey{account(), 71, 1}, 4, 4001},
        {mt5bridge::dispatch::ManagedPositionOwnerKind::unmanaged,
         0, 0, {100, 2, 1}, std::nullopt, 0, 5001},
    };
    require(external.record->valid(), "unmanaged-residue fixture is invalid");
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *candidate, fixture.reversal_record, external);
    require(result.status == mt5bridge::dispatch::
                                 ManagedInoutCloseAttributionStatus::
                                     unmanaged_position_residue &&
                !result.authorization,
            "unmanaged position residue was attributed to the target trade");
}

void rejects_partial_or_mismatched_close() {
    Fixture fixture;
    const auto partial = fixture.candidate(3);
    require(partial && partial->valid(), "partial structural candidate was not produced");
    MemoryOwnershipSource source;
    source.record = fixture.frontier();
    auto result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *partial, fixture.reversal_record, source);
    require(result.status == mt5bridge::dispatch::
                                 ManagedInoutCloseAttributionStatus::
                                     unsupported_partial_close &&
                !result.authorization,
            "partial INOUT close was authorized by the exclusive contract");

    auto foreign = fixture.reversal_record;
    foreign.deal_ticket = 999;
    require(foreign.valid(), "foreign broker fixture is invalid");
    const auto full = fixture.candidate();
    result = mt5bridge::dispatch::ManagedInoutCloseAttributionAuthorizer::assess(
        *full, foreign, source);
    require(result.status ==
                mt5bridge::dispatch::ManagedInoutCloseAttributionStatus::invalid_broker &&
                !result.authorization,
            "foreign broker deal reused a managed attribution candidate");
}

void keeps_frontier_digest_order_independent() {
    Fixture fixture;
    auto left = fixture.frontier();
    left.entries = {
        {mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
         71, 3, {300, 2, 1}, mt5bridge::OperationKey{account(), 71, 1}, 4, 4001},
        {mt5bridge::dispatch::ManagedPositionOwnerKind::managed_trade,
         71, 2, {200, 2, 1}, mt5bridge::OperationKey{account(), 71, 1}, 5, 4002},
    };
    auto right = left;
    std::swap(right.entries.front(), right.entries.back());
    require(left.valid() && right.valid(), "digest-order fixtures are invalid");
    require(mt5bridge::dispatch::managed_position_ownership_frontier_digest(left) ==
                mt5bridge::dispatch::managed_position_ownership_frontier_digest(right),
            "canonical ownership digest depends on entry order");
}

} // namespace

int main() {
    try {
        authorizes_exact_exclusive_full_close();
        rejects_missing_invalid_or_stale_frontier();
        rejects_competing_managed_or_unmanaged_ownership();
        rejects_partial_or_mismatched_close();
        keeps_frontier_digest_order_independent();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

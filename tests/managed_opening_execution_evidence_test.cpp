/// \file managed_opening_execution_evidence_test.cpp
/// \brief Verifies OPEN execution replay and cumulative-frontier semantics.

#include "managed_opening_execution_evidence.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
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

OperationKey operation(std::uint64_t operation_id = 9) {
    return {account(), 71, operation_id};
}

ManagedOpeningExecutionRequest request_for(
    const ManagedOpeningDealObservation &observation) {
    return {observation.operation_key, OperationKind::open,
            observation.operation_revision, observation.order_ticket,
            observation.position_identifier};
}

ManagedOpeningDealObservation observation(std::uint64_t deal_ticket = 5001,
                                          std::uint64_t source_revision = 4,
                                          double volume = 0.05) {
    return {operation(), 3, 3001, deal_ticket, 88001,
            kManagedOpeningRequiredDealFields, BrokerPositionDirection::buy,
            BrokerDealEntry::in, 1500, volume, 0.01,
            ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000},
            source_revision, 99, source_revision, 1000, 2000};
}

ManagedOpeningDealFact fact(ManagedOpeningDealObservation candidate) {
    const auto result = ManagedOpeningDealFact::from_observation(
        request_for(candidate), candidate);
    require(result.has_value(), "locally valid OPEN deal was rejected");
    return *result;
}

template <typename T, typename = void>
struct has_terminal_method : std::false_type {};

template <typename T>
struct has_terminal_method<T,
                           std::void_t<decltype(std::declval<const T &>().terminal())>>
    : std::true_type {};

std::string hex(const std::vector<std::uint8_t> &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

void validates_local_shape_without_claiming_authority() {
    const auto candidate = observation();
    require(validate_managed_opening_observation(request_for(candidate), candidate) ==
                ManagedOpeningExecutionValidationStatus::valid,
            "complete local OPEN join was rejected");
    const auto admitted = fact(candidate);
    require(admitted.valid(), "created OPEN fact is internally inconsistent");

    auto incomplete = candidate;
    incomplete.history_coverage_revision = 5;
    require(validate_managed_opening_observation(request_for(incomplete), incomplete) ==
                ManagedOpeningExecutionValidationStatus::incomplete_history,
            "mismatched sampled and coverage revisions were accepted");
    require(!ManagedOpeningDealFact::from_observation(
                 request_for(incomplete), incomplete),
            "incomplete history created a local fact");

    incomplete = candidate;
    incomplete.known_fields &=
        ~static_cast<std::uint64_t>(MT5BRIDGE_DEAL_KNOWN_TIME);
    require(validate_managed_opening_observation(request_for(incomplete), incomplete) ==
                ManagedOpeningExecutionValidationStatus::invalid_deal,
            "missing history field was accepted");

    incomplete = candidate;
    incomplete.entry = BrokerDealEntry::inout;
    require(validate_managed_opening_observation(request_for(incomplete), incomplete) ==
                ManagedOpeningExecutionValidationStatus::invalid_deal,
            "INOUT crossed the OPEN-only gate");

    incomplete = observation(5001, 4, 0.015);
    require(!ManagedOpeningDealFact::from_observation(
                 request_for(incomplete), incomplete),
            "fractional broker step created a fact");

    incomplete = candidate;
    incomplete.quantity_domain.canonical_scale = 10;
    require(validate_managed_opening_observation(request_for(incomplete), incomplete) ==
                ManagedOpeningExecutionValidationStatus::invalid_quantity,
            "invalid quantity domain passed local validation");
}

void rejects_mismatched_join_constraints() {
    const auto candidate = observation();
    auto request = request_for(candidate);
    request.operation_revision = 0;
    require(validate_managed_opening_observation(request, candidate) ==
                ManagedOpeningExecutionValidationStatus::invalid_request,
            "invalid OPEN query was accepted");

    request = request_for(candidate);
    request.operation_key.operation_id = 10;
    require(validate_managed_opening_observation(request, candidate) ==
                ManagedOpeningExecutionValidationStatus::operation_mismatch,
            "foreign operation joined to a deal");
    request = request_for(candidate);
    request.order_ticket = 3002;
    require(validate_managed_opening_observation(request, candidate) ==
                ManagedOpeningExecutionValidationStatus::order_mismatch,
            "foreign order joined to a deal");
    request = request_for(candidate);
    request.position_identifier = 88002;
    require(validate_managed_opening_observation(request, candidate) ==
                ManagedOpeningExecutionValidationStatus::position_mismatch,
            "foreign position joined to a deal");
}

void makes_repoll_and_restart_idempotent() {
    const auto committed = fact(observation());
    const auto original_bytes = committed.canonical_bytes();

    auto repoll = observation(5001, 5);
    repoll.graph_instance_id = 99;
    const auto repolled = fact(repoll);
    require(committed.admission_receipt().source_revision !=
                repolled.admission_receipt().source_revision,
            "repoll fixture did not change provenance");
    require(classify_managed_opening_replay(committed, repolled) ==
                ManagedOpeningExecutionReplayStatus::already_committed,
            "unchanged deal after repoll was not idempotent");

    auto restart = observation(5001, 6);
    restart.graph_instance_id = 100;
    const auto restarted = fact(restart);
    require(classify_managed_opening_replay(committed, restarted) ==
                ManagedOpeningExecutionReplayStatus::already_committed,
            "unchanged deal after graph restart was not idempotent");
    require(committed.canonical_bytes() == original_bytes,
            "replay rewrote the original admission receipt");
}

void rejects_conflicting_broker_payload() {
    const auto committed = fact(observation());
    const auto expect_conflict = [&](ManagedOpeningDealObservation changed,
                                     const char *message) {
        require(classify_managed_opening_replay(committed, fact(std::move(changed))) ==
                    ManagedOpeningExecutionReplayStatus::conflict,
                message);
    };

    expect_conflict(observation(5001, 5, 0.04),
                    "same deal with changed volume was not a conflict");
    auto changed = observation(5001, 5);
    changed.order_ticket = 3002;
    expect_conflict(changed, "same deal with changed order was not a conflict");
    changed = observation(5001, 5);
    changed.position_identifier = 88002;
    expect_conflict(changed, "same deal with changed position was not a conflict");
    changed = observation(5001, 5);
    changed.direction = BrokerPositionDirection::sell;
    expect_conflict(changed, "same deal with changed direction was not a conflict");
    changed = observation(5001, 5);
    changed.deal_time_msc = 1501;
    expect_conflict(changed, "same deal with changed timestamp was not a conflict");

    require(classify_managed_opening_replay(
                committed, fact(observation(5002, 5))) ==
                ManagedOpeningExecutionReplayStatus::new_execution,
            "different deal ticket was treated as replay");
}

void enforces_one_opening_identity_and_quantity_domain() {
    const auto first = fact(observation(5001, 4, 0.05));
    const auto second = fact(observation(5002, 4, 0.03));
    const auto same_batch = ManagedOpeningExecutionFrontier::create({first, second});
    require(same_batch && same_batch->cumulative_broker_units() == 8 &&
                same_batch->cumulative_managed_logical_units() == 8,
            "multiple fills from one observation revision were rejected");

    const auto expect_mixed_rejection = [&](ManagedOpeningDealObservation mixed,
                                            const char *message) {
        require(!ManagedOpeningExecutionFrontier::create({first, fact(std::move(mixed))}),
                message);
    };

    auto mixed = observation(5002, 5, 0.03);
    mixed.operation_key.account.login = 43;
    expect_mixed_rejection(mixed, "mixed accounts formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.operation_key.operation_id = 10;
    expect_mixed_rejection(mixed, "mixed operations formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.operation_revision = 4;
    expect_mixed_rejection(mixed, "mixed journal revisions formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.position_identifier = 88002;
    expect_mixed_rejection(mixed, "mixed positions formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.order_ticket = 3002;
    expect_mixed_rejection(mixed, "mixed orders formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.direction = BrokerPositionDirection::sell;
    expect_mixed_rejection(mixed, "mixed directions formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.quantity_domain = {2, 1, 2, 1000, 2000};
    expect_mixed_rejection(mixed, "mixed logical mappings formed one frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.observed_step = 0.001;
    mixed.quantity_domain = {3, 1, 1, 1000, 1000};
    expect_mixed_rejection(mixed, "mixed broker scales formed one frontier");

    const auto initial = ManagedOpeningExecutionFrontier::create({first});
    require(initial.has_value(), "single opening fact did not form a frontier");
    mixed = observation(5002, 5, 0.03);
    mixed.position_identifier = 88002;
    const auto incompatible = append_managed_opening_execution(*initial, fact(mixed));
    require(incompatible.status ==
                ManagedOpeningExecutionFrontierAppendStatus::incompatible_identity,
            "append accepted a foreign opening identity");
    const auto conflicting = append_managed_opening_execution(
        *initial, fact(observation(5001, 5, 0.04)));
    require(conflicting.status == ManagedOpeningExecutionFrontierAppendStatus::conflict,
            "append accepted conflicting payload for one deal ticket");

    auto huge_first = observation(6001, 6, 0.01);
    huge_first.quantity_domain = {
        2, 1, (std::numeric_limits<std::uint64_t>::max)(), 1,
        (std::numeric_limits<std::uint64_t>::max)()};
    auto huge_second = huge_first;
    huge_second.deal_ticket = 6002;
    require(!ManagedOpeningExecutionFrontier::create(
                 {fact(huge_first), fact(huge_second)}),
             "cumulative logical-unit overflow formed a frontier");

    auto broker_huge_first = observation(7001, 7, 9.3e18);
    broker_huge_first.observed_step = 1000000.0;
    broker_huge_first.quantity_domain = {
        0, 1000000, 1, (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)()};
    auto broker_huge_second = broker_huge_first;
    broker_huge_second.deal_ticket = 7002;
    require(!ManagedOpeningExecutionFrontier::create(
                 {fact(broker_huge_first), fact(broker_huge_second)}),
            "cumulative broker-unit overflow formed a frontier");
}

void extends_frontier_without_terminal_authority_or_rewrite() {
    static_assert(!has_terminal_method<ManagedOpeningExecutionFrontier>::value,
                  "execution frontier must not assert lifecycle terminality");
    const auto first = fact(observation(5001, 4, 0.05));
    const auto initial = ManagedOpeningExecutionFrontier::create({first});
    require(initial && initial->cumulative_managed_logical_units() == 5,
            "partial cumulative frontier was rejected");

    // The same cumulative five units can coexist with either a pending
    // remainder or a separately proven cancelled remainder. This contract
    // intentionally cannot distinguish or manufacture either lifecycle fact.
    const auto original = initial->canonical_bytes();
    const auto extended = append_managed_opening_execution(
        *initial, fact(observation(5002, 5, 0.05)));
    require(extended.appended() &&
                extended.frontier->cumulative_managed_logical_units() == 10,
            "late fill did not extend cumulative executions");
    require(initial->canonical_bytes() == original,
            "frontier extension rewrote its historical revision");

    const auto same_revision = append_managed_opening_execution(
        *initial, fact(observation(5002, 4, 0.03)));
    require(same_revision.appended(),
            "second partial fill from the same sampled revision was rejected");
    const auto duplicate = append_managed_opening_execution(*initial, first);
    require(duplicate.status == ManagedOpeningExecutionFrontierAppendStatus::duplicate,
            "duplicate execution was appended twice");
    const auto restarted_source = append_managed_opening_execution(
        *extended.frontier, fact(observation(5003, 1, 0.01)));
    require(restarted_source.appended(),
            "restart-local source revision was treated as globally stale");

    auto earlier_local_revision = observation(5004, 2, 0.01);
    earlier_local_revision.graph_instance_id = 101;
    const auto earlier_source = append_managed_opening_execution(
        *restarted_source.frontier, fact(earlier_local_revision));
    require(earlier_source.appended(),
            "independently valid earlier local revision was rejected");
}

void preserves_canonical_golden_vectors() {
    const auto entry = fact(observation());
    const auto frontier = ManagedOpeningExecutionFrontier::create({entry});
    require(frontier.has_value(), "golden frontier fixture was rejected");

    const std::string expected_fact =
        "23000000000000006d74356272696467652d6d616e616765642d6f70656e696e672d64"
        "65616c2d666163740100000000000000040000000000000064656d6f2a000000000000"
        "00470000000000000009000000000000000300000000000000b90b0000000000008913"
        "000000000000c15701000000000001000000000000000000000000000000dc05000000"
        "0000000200000000000000010000000000000005000000000000000500000000000000"
        "0500000000000000040000000000000063000000000000000400000000000000e80300"
        "0000000000d007000000000000";
    const std::string expected_frontier =
        "22000000000000006d74356272696467652d6d616e616765642d6f70656e696e672d66"
        "726f6e7469657201000000000000000500000000000000050000000000000001000000"
        "00000000df0000000000000023000000000000006d74356272696467652d6d616e6167"
        "65642d6f70656e696e672d6465616c2d66616374010000000000000004000000000000"
        "0064656d6f2a0000000000000047000000000000000900000000000000030000000000"
        "0000b90b0000000000008913000000000000c157010000000000010000000000000000"
        "00000000000000dc050000000000000200000000000000010000000000000005000000"
        "0000000005000000000000000500000000000000040000000000000063000000000000"
        "000400000000000000e803000000000000d007000000000000";
    const auto actual_fact = hex(entry.canonical_bytes());
    const auto actual_frontier = hex(frontier->canonical_bytes());
    if (actual_fact != expected_fact)
        std::cerr << "FACT_GOLDEN=" << actual_fact << '\n';
    if (actual_frontier != expected_frontier)
        std::cerr << "FRONTIER_GOLDEN=" << actual_frontier << '\n';
    require(actual_fact == expected_fact,
            "canonical deal-fact bytes changed without a version bump");
    require(actual_frontier == expected_frontier,
            "canonical frontier bytes changed without a version bump");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--negative-control")
            require(false, "intentional Release negative control");
        validates_local_shape_without_claiming_authority();
        rejects_mismatched_join_constraints();
        makes_repoll_and_restart_idempotent();
        rejects_conflicting_broker_payload();
        enforces_one_opening_identity_and_quantity_domain();
        extends_frontier_without_terminal_authority_or_rewrite();
        preserves_canonical_golden_vectors();
        std::cout << "managed opening execution evidence tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

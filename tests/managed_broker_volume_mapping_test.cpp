/// \file managed_broker_volume_mapping_test.cpp
/// \brief Exercises exact managed logical-unit to broker-volume conversion.

#include <mt5bridge/dispatch/managed_broker_volume_mapping.hpp>

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::ManagedSettlementProvenance managed_provenance() {
    return {11, 8, 8, 101};
}

mt5bridge::BrokerReversalProvenance broker_provenance() {
    return {11, 8, 12, 8, 12, 10, 202};
}

mt5bridge::ManagedDealSettlement settlement(std::uint64_t ticket,
                                             std::uint64_t units) {
    mt5bridge::ManagedDealSettlement result;
    result.key.account = {"Demo-Trade", 42};
    result.key.operation_key = {result.key.account, 71, 4};
    result.key.deal_ticket = ticket;
    result.source_operation_revision = 8;
    result.managed_logical_units = units;
    result.provenance = managed_provenance();
    require(result.valid(), "managed settlement fixture is invalid");
    return result;
}

mt5bridge::BrokerReversalRecord broker(std::uint64_t ticket) {
    mt5bridge::BrokerReversalRecord result;
    result.account = {"Demo-Trade", 42};
    result.margin_mode = mt5bridge::BrokerMarginMode::retail_netting;
    result.symbol = "EURUSD";
    result.deal_ticket = ticket;
    result.order_ticket = 900;
    result.deal_position_id = 901;
    result.deal_entry = mt5bridge::BrokerDealEntry::inout;
    result.pre_position_identifier = 901;
    result.pre_direction = mt5bridge::BrokerPositionDirection::buy;
    result.pre_volume = {500, 2, 1};
    result.post_position_identifier = 901;
    result.post_direction = mt5bridge::BrokerPositionDirection::sell;
    result.post_volume = {100, 2, 1};
    result.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    result.deal_volume = {600, 2, 1};
    result.broker_close_leg = result.pre_volume;
    result.broker_reverse_open_leg = result.post_volume;
    result.provenance = broker_provenance();
    require(result.valid(), "broker reversal fixture is invalid");
    return result;
}

void test_exact_scale_conversion() {
    const auto mapped = mt5bridge::map_managed_logical_units_to_broker_volume(
        250, {2}, {2, 25});
    require(mapped && mapped->units == 250 && mapped->scale == 2 &&
                mapped->step_units == 25,
            "equal decimal scales must preserve exact units");

    const auto finer = mt5bridge::map_managed_logical_units_to_broker_volume(
        3, {0}, {2, 100});
    require(finer && finer->units == 300,
            "finer broker scale must multiply with integer arithmetic");

    const auto coarser = mt5bridge::map_managed_logical_units_to_broker_volume(
        300, {2}, {0, 1});
    require(coarser && coarser->units == 3,
            "coarser broker scale must divide only exactly");
}

void test_non_exact_values_fail_closed() {
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                250, {2}, {0, 1}),
            "fractional coarsening must be rejected");
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                250, {2}, {2, 30}),
            "non-step-aligned conversion must be rejected");
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                (std::numeric_limits<std::uint64_t>::max)(), {0}, {1, 1}),
            "conversion overflow must be rejected");
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                0, {0}, {0, 1}),
            "zero logical units must be rejected");
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                1, {10}, {0, 1}),
            "unsupported logical scale must be rejected");
    require(!mt5bridge::map_managed_logical_units_to_broker_volume(
                1, {0}, {0, 0}),
            "missing broker step must be rejected");
}

void test_proof_binding_does_not_allocate_legs() {
    const auto managed = settlement(701, 250);
    const auto broker_record = broker(701);
    const auto mapped = mt5bridge::derive_managed_broker_volume_mapping(
        managed, broker_record, {2});
    require(mapped && mapped->valid(), "matching durable proofs must map");
    require(mapped->broker_volume().units == 250 &&
                !(mapped->broker_volume() == broker_record.broker_close_leg),
            "mapping must not silently consume the broker close leg");

    const auto managed_before = managed;
    const auto broker_before = broker_record;
    require(managed == managed_before && broker_record == broker_before,
            "mapping must not mutate source proofs");
}

void test_identity_mismatch_fails_closed() {
    auto mismatched_ticket = broker(702);
    require(!mt5bridge::derive_managed_broker_volume_mapping(
                settlement(701, 250), mismatched_ticket, {2}),
            "different deal tickets must not be bound");

    auto mismatched_account = broker(701);
    mismatched_account.account.login = 43;
    require(!mt5bridge::derive_managed_broker_volume_mapping(
                settlement(701, 250), mismatched_account, {2}),
            "different accounts must not be bound");
}

} // namespace

int main() {
    try {
        test_exact_scale_conversion();
        test_non_exact_values_fail_closed();
        test_proof_binding_does_not_allocate_legs();
        test_identity_mismatch_fails_closed();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "managed broker volume mapping tests passed\n";
    return 0;
}

/// \file broker_reversal_store_test.cpp
/// \brief Exercises durable broker-level reversal records and replay semantics.

#include <mt5bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::BrokerReversalObservation observation() {
    mt5bridge::BrokerReversalObservation value;
    value.account = {"Demo-Trade", 42};
    value.margin_mode = mt5bridge::BrokerMarginMode::retail_netting;
    value.symbol = "EURUSD";
    value.deal_ticket = 701;
    value.order_ticket = 702;
    value.deal_position_id = 800;
    value.deal_entry = mt5bridge::BrokerDealEntry::inout;
    value.pre_position_identifier = 800;
    value.pre_direction = mt5bridge::BrokerPositionDirection::buy;
    value.pre_volume = {500, 2, 1};
    value.post_position_identifier = 800;
    value.post_direction = mt5bridge::BrokerPositionDirection::sell;
    value.post_volume = {100, 2, 1};
    value.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    value.deal_volume = {600, 2, 1};
    value.provenance = {9, 10, 11, 10, 11, 11, 0xD00DFEED};
    return value;
}

mt5bridge::BrokerReversalRecord record() {
    const auto derived = mt5bridge::derive_broker_reversal_record(observation());
    require(derived.has_value(), "fresh broker observations were not derivable");
    return *derived;
}

void check_validation_boundaries() {
    auto value = record();
    require(value.valid(), "baseline broker reversal record is invalid");

    auto hedging = value;
    hedging.margin_mode = mt5bridge::BrokerMarginMode::retail_hedging;
    require(!hedging.valid(), "hedging reversal must fail closed");

    auto external_mutation = value;
    external_mutation.post_volume = {200, 2, 1};
    require(!external_mutation.valid(), "concurrent post mutation must fail closed");

    auto incomplete = value;
    incomplete.provenance.evidence_digest = 0;
    require(!incomplete.valid(), "incomplete provenance must fail closed");

    auto contradictory = value;
    contradictory.broker_close_leg = {400, 2, 1};
    require(!contradictory.valid(), "contradictory decomposition must fail closed");

    auto changed_identity = value;
    changed_identity.post_position_identifier = 801;
    require(!changed_identity.valid(),
            "position identity replacement must fail closed");

    auto mismatched_deal_position = value;
    mismatched_deal_position.deal_position_id = 801;
    require(!mismatched_deal_position.valid(),
            "deal position identity mismatch must fail closed");

    auto non_inout = observation();
    non_inout.deal_entry = mt5bridge::BrokerDealEntry::in;
    require(!mt5bridge::derive_broker_reversal_record(non_inout).has_value(),
            "DEAL_ENTRY_IN must fail closed");
    non_inout.deal_entry = mt5bridge::BrokerDealEntry::out;
    require(!mt5bridge::derive_broker_reversal_record(non_inout).has_value(),
            "DEAL_ENTRY_OUT must fail closed");
}

void check_durable_replay() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_broker_reversal_store_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto expected = record();

    {
        mt5bridge::WindowsFileBrokerReversalStore store(root);
        require(store.ready(), "broker reversal store did not open");
        require(store.commit(expected) ==
                    mt5bridge::BrokerReversalCommitStatus::committed,
                "initial broker reversal commit failed");
        require(store.commit(expected) ==
                    mt5bridge::BrokerReversalCommitStatus::already_committed,
                "identical broker reversal replay was not idempotent");

        auto conflict = expected;
        conflict.provenance.evidence_digest += 1;
        require(store.commit(conflict) ==
                    mt5bridge::BrokerReversalCommitStatus::conflict,
                "different decomposition did not conflict");

        const auto loaded = store.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "durable broker reversal load did not round-trip");
        const auto scan = store.scan();
        require(scan.complete() && scan.records.size() == 1 &&
                    scan.records.front() == expected,
                "broker reversal scan did not return one durable record");
    }

    {
        mt5bridge::WindowsFileBrokerReversalStore recovered(root);
        require(recovered.ready(), "broker reversal store did not reopen");
        const auto loaded = recovered.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "restart recovery did not replay durable decomposition");
    }
    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs broker reversal validation and durable replay checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_validation_boundaries();
        check_durable_replay();
        std::cout << "broker reversal store checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "broker reversal store checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

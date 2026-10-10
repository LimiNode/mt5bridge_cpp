/// \file broker_allocation_envelope_store_test.cpp
/// \brief Exercises proof-gated broker-allocation envelopes and durable replay.

#include <mt5bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::BrokerReversalRecord broker_record() {
    mt5bridge::BrokerReversalObservation observation;
    observation.account = {"Demo-Trade", 42};
    observation.margin_mode = mt5bridge::BrokerMarginMode::retail_netting;
    observation.symbol = "EURUSD";
    observation.deal_ticket = 701;
    observation.order_ticket = 702;
    observation.deal_position_id = 800;
    observation.deal_entry = mt5bridge::BrokerDealEntry::inout;
    observation.pre_position_identifier = 800;
    observation.pre_direction = mt5bridge::BrokerPositionDirection::buy;
    observation.pre_volume = {500, 2, 1};
    observation.post_position_identifier = 800;
    observation.post_direction = mt5bridge::BrokerPositionDirection::sell;
    observation.post_volume = {100, 2, 1};
    observation.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    observation.deal_volume = {600, 2, 1};
    observation.provenance = {9, 10, 11, 10, 11, 11, 0xD00DFEED};
    const auto result = mt5bridge::derive_broker_reversal_record(observation);
    require(result.has_value(), "broker proof was not derivable");
    return *result;
}

mt5bridge::BrokerAllocationEnvelope envelope(
    const mt5bridge::BrokerReversalRecord &broker) {
    mt5bridge::BrokerAllocationEnvelope value;
    value.account = broker.account;
    value.broker_key = broker.key();
    value.broker_provenance = broker.provenance;
    value.broker_close_leg = broker.broker_close_leg;
    value.broker_reverse_open_leg = broker.broker_reverse_open_leg;
    value.unallocated_close_leg = broker.broker_close_leg;
    value.unallocated_reverse_open_leg = broker.broker_reverse_open_leg;
    return value;
}

class MemoryBrokerStore final : public mt5bridge::DurableBrokerReversalStore {
public:
    explicit MemoryBrokerStore(std::optional<mt5bridge::BrokerReversalRecord> value = {})
        : record_(std::move(value)) {}

    mt5bridge::BrokerReversalCommitStatus commit(
        const mt5bridge::BrokerReversalRecord &value) override {
        if (!value.valid())
            return mt5bridge::BrokerReversalCommitStatus::invalid_record;
        if (!record_) {
            record_ = value;
            return mt5bridge::BrokerReversalCommitStatus::committed;
        }
        return *record_ == value ? mt5bridge::BrokerReversalCommitStatus::already_committed
                                 : mt5bridge::BrokerReversalCommitStatus::conflict;
    }

    mt5bridge::BrokerReversalLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (!record_ || !(record_->key() == key))
            return {mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt};
        return {mt5bridge::BrokerReversalLoadStatus::found, record_};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        if (!record_)
            return {mt5bridge::BrokerReversalScanStatus::complete, {}};
        return {mt5bridge::BrokerReversalScanStatus::complete, {*record_}};
    }

private:
    std::optional<mt5bridge::BrokerReversalRecord> record_;
};

class MemoryEnvelopeStore final : public mt5bridge::DurableBrokerAllocationStore {
public:
    mt5bridge::BrokerAllocationCommitStatus commit(
        const mt5bridge::BrokerAllocationEnvelope &value,
        const mt5bridge::DurableBrokerReversalStore &broker_store) override {
        const auto proof =
            mt5bridge::validate_broker_allocation_envelope(value, broker_store);
        if (proof != mt5bridge::BrokerAllocationProofStatus::valid) {
            switch (proof) {
            case mt5bridge::BrokerAllocationProofStatus::invalid_record:
                return mt5bridge::BrokerAllocationCommitStatus::invalid_record;
            case mt5bridge::BrokerAllocationProofStatus::missing_broker_record:
                return mt5bridge::BrokerAllocationCommitStatus::missing_broker_record;
            case mt5bridge::BrokerAllocationProofStatus::invalid_broker_record:
                return mt5bridge::BrokerAllocationCommitStatus::invalid_broker_record;
            case mt5bridge::BrokerAllocationProofStatus::io_error:
                return mt5bridge::BrokerAllocationCommitStatus::io_error;
            case mt5bridge::BrokerAllocationProofStatus::valid:
                break;
            }
        }
        if (!record_) {
            record_ = value;
            return mt5bridge::BrokerAllocationCommitStatus::committed;
        }
        return *record_ == value
                   ? mt5bridge::BrokerAllocationCommitStatus::already_committed
                   : mt5bridge::BrokerAllocationCommitStatus::conflict;
    }

    mt5bridge::BrokerAllocationLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (!record_ || !(record_->key() == key))
            return {mt5bridge::BrokerAllocationLoadStatus::not_found, std::nullopt};
        return {mt5bridge::BrokerAllocationLoadStatus::found, record_};
    }

    mt5bridge::BrokerAllocationScanResult scan() const override {
        if (!record_)
            return {mt5bridge::BrokerAllocationScanStatus::complete, {}};
        return {mt5bridge::BrokerAllocationScanStatus::complete, {*record_}};
    }

private:
    std::optional<mt5bridge::BrokerAllocationEnvelope> record_;
};

void check_validation_and_proof_gate() {
    const auto broker = broker_record();
    const auto expected = envelope(broker);
    require(expected.valid(), "baseline broker allocation envelope is invalid");

    MemoryBrokerStore broker_store(broker);
    MemoryEnvelopeStore envelope_store;
    require(envelope_store.commit(expected, broker_store) ==
                mt5bridge::BrokerAllocationCommitStatus::committed,
            "proof-gated envelope commit failed");
    require(envelope_store.commit(expected, broker_store) ==
                mt5bridge::BrokerAllocationCommitStatus::already_committed,
            "identical envelope replay was not idempotent");

    auto partial = expected;
    partial.unallocated_close_leg.units = 400;
    require(!partial.valid(), "partial managed attribution crossed envelope boundary");
    require(envelope_store.commit(partial, broker_store) ==
                mt5bridge::BrokerAllocationCommitStatus::invalid_record,
            "partial managed attribution was accepted as an envelope");

    MemoryBrokerStore missing;
    MemoryEnvelopeStore missing_destination;
    require(missing_destination.commit(expected, missing) ==
                mt5bridge::BrokerAllocationCommitStatus::missing_broker_record,
            "envelope committed without durable broker proof");

    auto wrong_provenance = expected;
    wrong_provenance.broker_provenance.evidence_digest += 1;
    require(missing_destination.commit(wrong_provenance, broker_store) ==
                mt5bridge::BrokerAllocationCommitStatus::invalid_broker_record,
            "envelope accepted mismatched broker provenance");
}

void check_durable_replay() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_broker_allocation_store_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto broker = broker_record();
    const auto expected = envelope(broker);
    {
        mt5bridge::WindowsFileBrokerAllocationStore store(root);
        MemoryBrokerStore broker_store(broker);
        require(store.ready(), "broker allocation store did not open");
        require(store.commit(expected, broker_store) ==
                    mt5bridge::BrokerAllocationCommitStatus::committed,
                "initial envelope commit failed");
        require(store.commit(expected, broker_store) ==
                    mt5bridge::BrokerAllocationCommitStatus::already_committed,
                "envelope replay was not idempotent");
        const auto loaded = store.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "envelope did not round-trip through durable storage");
        const auto scan = store.scan();
        require(scan.complete() && scan.records.size() == 1 &&
                    scan.records.front() == expected,
                "envelope scan did not return one durable record");
    }
    {
        mt5bridge::WindowsFileBrokerAllocationStore recovered(root);
        require(recovered.ready(), "broker allocation store did not reopen");
        const auto loaded = recovered.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "envelope restart recovery did not round-trip");
    }
    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs broker-allocation envelope proof and persistence checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_validation_and_proof_gate();
        check_durable_replay();
        std::cout << "broker allocation envelope checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "broker allocation envelope checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

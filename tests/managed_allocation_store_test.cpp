/// \file managed_allocation_store_test.cpp
/// \brief Exercises proof-gated managed allocation links and durable replay.

#include <mt5bridge.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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

mt5bridge::ManagedAllocationRecord allocation_record(
    const mt5bridge::BrokerReversalRecord &broker) {
    mt5bridge::ManagedAllocationRecord value;
    value.account = broker.account;
    value.broker_key = broker.key();
    value.operation_key = {broker.account, 17, 2};
    value.broker_provenance = broker.provenance;
    value.managed_close_leg = {200, 2, 1};
    value.managed_reverse_open_leg = {0, 2, 1};
    value.unallocated_close_leg = {300, 2, 1};
    value.unallocated_reverse_open_leg = {100, 2, 1};
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

class MemoryAllocationStore final : public mt5bridge::DurableManagedAllocationStore {
public:
    mt5bridge::ManagedAllocationCommitStatus commit(
        const mt5bridge::ManagedAllocationRecord &value,
        const mt5bridge::DurableBrokerReversalStore &broker_store) override {
        const auto proof =
            mt5bridge::validate_managed_allocation_proof(value, broker_store);
        if (proof != mt5bridge::ManagedAllocationProofStatus::valid) {
            switch (proof) {
            case mt5bridge::ManagedAllocationProofStatus::invalid_record:
                return mt5bridge::ManagedAllocationCommitStatus::invalid_record;
            case mt5bridge::ManagedAllocationProofStatus::missing_broker_record:
                return mt5bridge::ManagedAllocationCommitStatus::missing_broker_record;
            case mt5bridge::ManagedAllocationProofStatus::invalid_broker_record:
                return mt5bridge::ManagedAllocationCommitStatus::invalid_broker_record;
            case mt5bridge::ManagedAllocationProofStatus::io_error:
                return mt5bridge::ManagedAllocationCommitStatus::io_error;
            case mt5bridge::ManagedAllocationProofStatus::valid:
                break;
            }
        }
        if (!record_) {
            record_ = value;
            return mt5bridge::ManagedAllocationCommitStatus::committed;
        }
        return *record_ == value
                   ? mt5bridge::ManagedAllocationCommitStatus::already_committed
                   : mt5bridge::ManagedAllocationCommitStatus::conflict;
    }

    mt5bridge::ManagedAllocationLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (!record_ || !(record_->key() == key))
            return {mt5bridge::ManagedAllocationLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedAllocationLoadStatus::found, record_};
    }

    mt5bridge::ManagedAllocationScanResult scan() const override {
        if (!record_)
            return {mt5bridge::ManagedAllocationScanStatus::complete, {}};
        return {mt5bridge::ManagedAllocationScanStatus::complete, {*record_}};
    }

private:
    std::optional<mt5bridge::ManagedAllocationRecord> record_;
};

void check_validation_and_proof_gate() {
    const auto broker = broker_record();
    const auto allocation = allocation_record(broker);
    require(allocation.valid(), "baseline managed allocation is invalid");

    MemoryBrokerStore broker_store(broker);
    MemoryAllocationStore allocation_store;
    require(mt5bridge::commit_managed_allocation(allocation, broker_store,
                                                  allocation_store) ==
                mt5bridge::ManagedAllocationCommitStatus::committed,
            "proof-gated allocation commit failed");
    require(mt5bridge::commit_managed_allocation(allocation, broker_store,
                                                  allocation_store) ==
                mt5bridge::ManagedAllocationCommitStatus::already_committed,
            "identical allocation replay was not idempotent");

    auto conflicting = allocation;
    conflicting.operation_key.trade_id = 18;
    require(conflicting.valid(), "changed allocation should remain locally valid");
    require(mt5bridge::commit_managed_allocation(conflicting, broker_store,
                                                  allocation_store) ==
                mt5bridge::ManagedAllocationCommitStatus::conflict,
            "second managed owner silently replaced the allocation");

    auto over_allocated = allocation;
    over_allocated.managed_close_leg.units = 600;
    over_allocated.unallocated_close_leg.units = 0;
    require(over_allocated.valid(), "over-allocation should reach proof gate");
    require(mt5bridge::commit_managed_allocation(over_allocated, broker_store,
                                                  allocation_store) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_record,
            "allocation exceeded broker close leg");

    MemoryBrokerStore missing;
    MemoryAllocationStore missing_destination;
    require(mt5bridge::commit_managed_allocation(allocation, missing,
                                                  missing_destination) ==
                mt5bridge::ManagedAllocationCommitStatus::missing_broker_record,
            "allocation committed without durable broker proof");

    auto wrong_provenance = allocation;
    wrong_provenance.broker_provenance.evidence_digest += 1;
    require(mt5bridge::commit_managed_allocation(wrong_provenance, broker_store,
                                                  missing_destination) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_broker_record,
            "allocation accepted a mismatched broker provenance");
}

void check_durable_replay() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_allocation_store_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto broker = broker_record();
    const auto expected = allocation_record(broker);
    {
        mt5bridge::WindowsFileManagedAllocationStore store(root);
        MemoryBrokerStore broker_store(broker);
        require(store.ready(), "managed allocation store did not open");
        require(store.commit(expected, broker_store) ==
                    mt5bridge::ManagedAllocationCommitStatus::committed,
                "initial allocation commit failed");
        require(store.commit(expected, broker_store) ==
                    mt5bridge::ManagedAllocationCommitStatus::already_committed,
                "allocation replay was not idempotent");
        auto conflict = expected;
        conflict.operation_key.trade_id += 1;
        require(store.commit(conflict, broker_store) ==
                    mt5bridge::ManagedAllocationCommitStatus::conflict,
                "different allocation did not conflict");
        const auto loaded = store.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "allocation did not round-trip through durable storage");
        const auto scan = store.scan();
        require(scan.complete() && scan.records.size() == 1 &&
                    scan.records.front() == expected,
                "allocation scan did not return one durable link");
    }
    {
        mt5bridge::WindowsFileManagedAllocationStore recovered(root);
        require(recovered.ready(), "allocation store did not reopen");
        const auto loaded = recovered.load(expected.key());
        require(loaded.found() && *loaded.record == expected,
                "allocation restart recovery did not round-trip");
    }
    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs managed-allocation proof and persistence checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_validation_and_proof_gate();
        check_durable_replay();
        std::cout << "managed allocation store checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "managed allocation store checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

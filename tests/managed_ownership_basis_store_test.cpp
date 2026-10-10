/// \file managed_ownership_basis_store_test.cpp
/// \brief Exercises proof-gated managed ownership bases and durable replay.

#include <mt5bridge.hpp>

#include "support/memory_journal_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::BrokerReversalRecord broker_record() {
    mt5bridge::BrokerReversalObservation observation;
    observation.account = account();
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
    require(result.has_value(), "broker reversal proof was not derivable");
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

mt5bridge::OperationRecord settled_close(mt5bridge::OperationKey key,
                                         std::uint64_t settled_volume = 5,
                                         std::uint64_t revision = 8) {
    const auto baseline = mt5bridge::ReconciliationBaseline::restore(
        key.account, 4, 7, 1, 2, 3, 7);
    require(baseline.has_value(), "operation baseline was not restorable");

    mt5bridge::ReconciliationDescriptor descriptor{
        key.account,
        *baseline,
        {mt5bridge::require_history_deal(701, mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::filled};
    descriptor.trade_id = key.trade_id;
    descriptor.operation_id = key.operation_id;
    descriptor.requested_volume = settled_volume;

    mt5bridge::OperationRecord record;
    record.key = key;
    record.operation_kind = mt5bridge::OperationKind::close;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = mt5bridge::OperationState::filled;
    record.journal_state = mt5bridge::JournalState::reconciling;
    record.revision = revision;
    record.fencing_token = 3;
    record.reconciliation_descriptor = std::move(descriptor);
    record.settled_volume = settled_volume;
    require(record.valid(), "settled close operation fixture is invalid");
    return record;
}

mt5bridge::ManagedOwnershipBasis basis(
    const mt5bridge::BrokerAllocationEnvelope &source,
    const mt5bridge::OperationRecord &operation) {
    mt5bridge::ManagedOwnershipBasis value;
    value.account = source.account;
    value.basis_key = {source.broker_key, operation.key};
    value.broker_provenance = source.broker_provenance;
    value.operation_revision = operation.revision;
    return value;
}

using mt5bridge_test_support::MemoryJournalStore;

class MemoryEnvelopeStore final : public mt5bridge::DurableBrokerAllocationStore {
public:
    explicit MemoryEnvelopeStore(
        std::optional<mt5bridge::BrokerAllocationEnvelope> value = {})
        : record_(std::move(value)) {}

    mt5bridge::BrokerAllocationCommitStatus commit(
        const mt5bridge::BrokerAllocationEnvelope &,
        const mt5bridge::DurableBrokerReversalStore &) override {
        return mt5bridge::BrokerAllocationCommitStatus::io_error;
    }

    mt5bridge::BrokerAllocationLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        if (load_status != mt5bridge::BrokerAllocationLoadStatus::found)
            return {load_status, std::nullopt};
        if (!record_ || !(record_->key() == key))
            return {mt5bridge::BrokerAllocationLoadStatus::not_found, std::nullopt};
        return {mt5bridge::BrokerAllocationLoadStatus::found, record_};
    }

    mt5bridge::BrokerAllocationScanResult scan() const override {
        if (!record_)
            return {mt5bridge::BrokerAllocationScanStatus::complete, {}};
        return {mt5bridge::BrokerAllocationScanStatus::complete, {*record_}};
    }

    mt5bridge::BrokerAllocationLoadStatus load_status =
        mt5bridge::BrokerAllocationLoadStatus::found;

private:
    std::optional<mt5bridge::BrokerAllocationEnvelope> record_;
};

class MemoryBasisStore final : public mt5bridge::DurableManagedOwnershipBasisStore {
public:
    mt5bridge::ManagedOwnershipBasisCommitStatus commit(
        const mt5bridge::ManagedOwnershipBasis &value,
        const mt5bridge::DurableJournalStore &journal_store,
        const mt5bridge::DurableBrokerAllocationStore &allocation_store) override {
        const auto proof = mt5bridge::validate_managed_ownership_basis(
            value, journal_store, allocation_store);
        switch (proof) {
        case mt5bridge::ManagedOwnershipBasisProofStatus::invalid_record:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_record;
        case mt5bridge::ManagedOwnershipBasisProofStatus::missing_operation_record:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::missing_operation_record;
        case mt5bridge::ManagedOwnershipBasisProofStatus::invalid_operation_record:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_operation_record;
        case mt5bridge::ManagedOwnershipBasisProofStatus::missing_allocation_envelope:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::missing_allocation_envelope;
        case mt5bridge::ManagedOwnershipBasisProofStatus::invalid_allocation_envelope:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_allocation_envelope;
        case mt5bridge::ManagedOwnershipBasisProofStatus::io_error:
            return mt5bridge::ManagedOwnershipBasisCommitStatus::io_error;
        case mt5bridge::ManagedOwnershipBasisProofStatus::valid:
            break;
        }
        const auto found = records_.find(value.key());
        if (found == records_.end()) {
            records_.emplace(value.key(), value);
            return mt5bridge::ManagedOwnershipBasisCommitStatus::committed;
        }
        return found->second == value
                   ? mt5bridge::ManagedOwnershipBasisCommitStatus::already_committed
                   : mt5bridge::ManagedOwnershipBasisCommitStatus::conflict;
    }

    mt5bridge::ManagedOwnershipBasisLoadResult load(
        const mt5bridge::ManagedOwnershipBasisKey &key) const override {
        const auto found = records_.find(key);
        if (found == records_.end())
            return {mt5bridge::ManagedOwnershipBasisLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedOwnershipBasisLoadStatus::found, found->second};
    }

    mt5bridge::ManagedOwnershipBasisScanResult scan() const override {
        mt5bridge::ManagedOwnershipBasisScanResult result;
        result.status = mt5bridge::ManagedOwnershipBasisScanStatus::complete;
        for (const auto &entry : records_)
            result.records.push_back(entry.second);
        return result;
    }

private:
    std::map<mt5bridge::ManagedOwnershipBasisKey,
             mt5bridge::ManagedOwnershipBasis> records_;
};

void check_proof_gate() {
    const auto broker = broker_record();
    const auto allocation = envelope(broker);
    const auto operation = settled_close({account(), 17, 2});
    const auto expected = basis(allocation, operation);
    require(expected.valid(), "baseline managed ownership basis is invalid");

    MemoryJournalStore journal;
    journal.records.emplace(operation.key, operation);
    MemoryEnvelopeStore envelopes(allocation);
    MemoryBasisStore destination;
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::committed,
            "proof-gated ownership basis commit failed");
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::already_committed,
            "identical ownership basis replay was not idempotent");

    auto stale_revision = expected;
    stale_revision.operation_revision -= 1;
    require(destination.commit(stale_revision, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_operation_record,
            "stale operation revision authorized an ownership basis");

    auto wrong_deal_operation = operation;
    wrong_deal_operation.reconciliation_descriptor->predicates = {
        mt5bridge::require_history_deal(999, mt5bridge::ObservationWindow{1000, 2000})};
    require(wrong_deal_operation.valid(), "wrong-deal operation fixture is invalid");
    journal.records[operation.key] = wrong_deal_operation;
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_operation_record,
            "operation without the target deal authorized ownership");

    auto multiple_deal_operation = operation;
    multiple_deal_operation.reconciliation_descriptor->predicates = {
        mt5bridge::require_history_deal(701, mt5bridge::ObservationWindow{1000, 2000}),
        mt5bridge::require_history_deal(999, mt5bridge::ObservationWindow{1000, 2000})};
    require(multiple_deal_operation.valid(), "multiple-deal operation fixture is invalid");
    journal.records[operation.key] = multiple_deal_operation;
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_operation_record,
            "operation with multiple deal predicates authorized ambiguous ownership");

    auto open_operation = operation;
    open_operation.operation_kind = mt5bridge::OperationKind::open;
    require(open_operation.valid(), "open operation fixture is invalid");
    journal.records[operation.key] = open_operation;
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::invalid_operation_record,
            "open operation authorized reversal close ownership");

    journal.records.clear();
    require(destination.commit(expected, journal, envelopes) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::missing_operation_record,
            "basis committed without a durable operation");

    journal.records.emplace(operation.key, operation);
    MemoryEnvelopeStore missing_envelope;
    require(destination.commit(expected, journal, missing_envelope) ==
                mt5bridge::ManagedOwnershipBasisCommitStatus::missing_allocation_envelope,
            "basis committed without a durable broker envelope");
}

void check_durable_replay_and_composite_identity() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_ownership_basis_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto allocation = envelope(broker_record());
    const auto operation_a = settled_close({account(), 17, 2});
    const auto operation_b = settled_close({account(), 18, 4});
    const auto basis_a = basis(allocation, operation_a);
    const auto basis_b = basis(allocation, operation_b);

    MemoryJournalStore journal;
    journal.records.emplace(operation_a.key, operation_a);
    journal.records.emplace(operation_b.key, operation_b);
    MemoryEnvelopeStore envelopes(allocation);
    {
        mt5bridge::WindowsFileManagedOwnershipBasisStore store(root);
        require(store.ready(), "managed ownership basis store did not open");
        require(store.commit(basis_a, journal, envelopes) ==
                    mt5bridge::ManagedOwnershipBasisCommitStatus::committed,
                "first durable basis commit failed");
        require(store.commit(basis_b, journal, envelopes) ==
                    mt5bridge::ManagedOwnershipBasisCommitStatus::committed,
                "second operation for one reversal was not independently addressable");
        require(store.commit(basis_a, journal, envelopes) ==
                    mt5bridge::ManagedOwnershipBasisCommitStatus::already_committed,
                "durable basis replay was not idempotent");
        const auto scan = store.scan();
        require(scan.complete() && scan.records.size() == 2,
                "composite-key basis scan did not preserve both operations");
    }
    {
        mt5bridge::WindowsFileManagedOwnershipBasisStore recovered(root);
        require(recovered.ready(), "managed ownership basis store did not reopen");
        const auto loaded_a = recovered.load(basis_a.key());
        const auto loaded_b = recovered.load(basis_b.key());
        require(loaded_a.found() && *loaded_a.record == basis_a && loaded_b.found() &&
                    *loaded_b.record == basis_b,
                "managed ownership bases did not survive restart");
    }
    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs managed-ownership proof and persistence checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        check_proof_gate();
        check_durable_replay_and_composite_identity();
        std::cout << "managed ownership basis checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "managed ownership basis checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

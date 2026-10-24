/// \file managed_settlement_preparation_store_test.cpp
/// \brief Exercises durable staged settlement preparation and restart finalization.

#include "managed_deal_settlement_producer.hpp"

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>
#include <mt5bridge/dispatch/file_managed_deal_settlement_store.hpp>
#include <mt5bridge/dispatch/file_managed_ownership_basis_store.hpp>
#include <mt5bridge/dispatch/file_managed_settlement_preparation_store.hpp>
#include <mt5bridge/dispatch/managed_staged_allocation_admission.hpp>

#include "file_store_support.hpp"
#include "support/memory_journal_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::BrokerReversalProvenance broker_provenance() {
    return {11, 8, 12, 8, 12, 10, 202};
}

Mt5DealSnapshot deal_snapshot() {
    Mt5DealSnapshot value{};
    value.ticket = 701;
    value.order_ticket = 900;
    value.position_id = 901;
    value.entry = 1;
    value.volume = 2.0;
    value.time_msc = 1500;
    value.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                         MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                         MT5BRIDGE_DEAL_KNOWN_ENTRY |
                         MT5BRIDGE_DEAL_KNOWN_VOLUME |
                         MT5BRIDGE_DEAL_KNOWN_TIME;
    return value;
}

mt5bridge::BrokerReversalRecord broker_record() {
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
    result.pre_volume = {600, 2, 1};
    result.post_position_identifier = 800;
    result.post_direction = mt5bridge::BrokerPositionDirection::sell;
    result.post_volume = {100, 2, 1};
    result.deal_direction = mt5bridge::BrokerPositionDirection::sell;
    result.deal_volume = {700, 2, 1};
    result.broker_close_leg = result.pre_volume;
    result.broker_reverse_open_leg = result.post_volume;
    result.provenance = broker_provenance();
    require(result.valid(), "broker reversal fixture is invalid");
    return result;
}

mt5bridge::BrokerReversalRecord broker_record_for(std::uint64_t deal_ticket,
                                                   std::uint64_t evidence_digest) {
    auto result = broker_record();
    result.deal_ticket = deal_ticket;
    result.order_ticket += deal_ticket - 701;
    result.provenance.evidence_digest = evidence_digest;
    require(result.valid(), "parameterized broker reversal fixture is invalid");
    return result;
}

class MemoryBrokerStore final : public mt5bridge::DurableBrokerReversalStore {
public:
    mt5bridge::BrokerReversalCommitStatus commit(
        const mt5bridge::BrokerReversalRecord &) override {
        return mt5bridge::BrokerReversalCommitStatus::io_error;
    }

    mt5bridge::BrokerReversalLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::not_found, std::nullopt}
                   : mt5bridge::BrokerReversalLoadResult{
                         mt5bridge::BrokerReversalLoadStatus::found, found->second};
    }

    mt5bridge::BrokerReversalScanResult scan() const override {
        mt5bridge::BrokerReversalScanResult result;
        result.status = mt5bridge::BrokerReversalScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerReversalRecord> records;
};

class MemoryEnvelopeStore final : public mt5bridge::DurableBrokerAllocationStore {
public:
    mt5bridge::BrokerAllocationCommitStatus commit(
        const mt5bridge::BrokerAllocationEnvelope &,
        const mt5bridge::DurableBrokerReversalStore &) override {
        return mt5bridge::BrokerAllocationCommitStatus::io_error;
    }

    mt5bridge::BrokerAllocationLoadResult load(
        const mt5bridge::BrokerReversalKey &key) const override {
        const auto found = records.find(key);
        return found == records.end()
                   ? mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::not_found, std::nullopt}
                   : mt5bridge::BrokerAllocationLoadResult{
                         mt5bridge::BrokerAllocationLoadStatus::found, found->second};
    }

    mt5bridge::BrokerAllocationScanResult scan() const override {
        mt5bridge::BrokerAllocationScanResult result;
        result.status = mt5bridge::BrokerAllocationScanStatus::complete;
        for (const auto &item : records)
            result.records.push_back(item.second);
        return result;
    }

    std::map<mt5bridge::BrokerReversalKey, mt5bridge::BrokerAllocationEnvelope> records;
};

class TestLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &requested) const override {
        return requested == owned_account ? std::optional<std::uint64_t>(token)
                                          : std::nullopt;
    }

    bool with_fencing_capability(
        const mt5bridge::AccountKey &requested, std::uint64_t expected_token,
        const std::function<void()> &publication) const override {
        if (!(requested == owned_account) || expected_token != token || !publication)
            return false;
        publication();
        return true;
    }

    mt5bridge::AccountKey owned_account = account();
    std::uint64_t token = 7;
};

class MemorySettlementStore final : public mt5bridge::DurableManagedDealSettlementStore {
public:
    mt5bridge::ManagedDealSettlementCommitStatus commit(
        const mt5bridge::ManagedDealSettlementCommit &,
        const mt5bridge::DurableJournalStore &) override {
        return mt5bridge::ManagedDealSettlementCommitStatus::io_error;
    }

    mt5bridge::ManagedDealSettlementLoadResult load(
        const mt5bridge::ManagedDealSettlementKey &key) const override {
        const auto found = std::find_if(
            facts.begin(), facts.end(),
            [&key](const auto &candidate) { return candidate.key == key; });
        if (found == facts.end())
            return {mt5bridge::ManagedDealSettlementLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedDealSettlementLoadStatus::found, *found};
    }

    mt5bridge::ManagedSettlementFrontierLoadResult load_frontier(
        const mt5bridge::OperationKey &key, std::uint64_t revision) const override {
        const auto found = std::find_if(
            frontiers.begin(), frontiers.end(),
            [&key, revision](const auto &candidate) {
                return candidate.operation_key == key &&
                       candidate.operation_revision == revision;
            });
        if (found == frontiers.end())
            return {mt5bridge::ManagedSettlementFrontierLoadStatus::not_found,
                    std::nullopt};
        return {mt5bridge::ManagedSettlementFrontierLoadStatus::found, *found};
    }

    std::vector<mt5bridge::ManagedDealSettlement> facts;
    std::vector<mt5bridge::ManagedSettlementFrontier> frontiers;
};

class MemoryBasisStore final : public mt5bridge::DurableManagedOwnershipBasisStore {
public:
    mt5bridge::ManagedOwnershipBasisCommitStatus commit(
        const mt5bridge::ManagedOwnershipBasis &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableBrokerAllocationStore &) override {
        return mt5bridge::ManagedOwnershipBasisCommitStatus::io_error;
    }

    mt5bridge::ManagedOwnershipBasisLoadResult load(
        const mt5bridge::ManagedOwnershipBasisKey &key) const override {
        const auto found = std::find_if(
            bases.begin(), bases.end(),
            [&key](const auto &candidate) { return candidate.key() == key; });
        if (found == bases.end())
            return {mt5bridge::ManagedOwnershipBasisLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedOwnershipBasisLoadStatus::found, *found};
    }

    mt5bridge::ManagedOwnershipBasisScanResult scan() const override {
        mt5bridge::ManagedOwnershipBasisScanResult result;
        result.status = mt5bridge::ManagedOwnershipBasisScanStatus::complete;
        result.records = bases;
        return result;
    }

    std::vector<mt5bridge::ManagedOwnershipBasis> bases;
};

class MemoryAllocationStore final : public mt5bridge::DurableManagedAllocationStore {
public:
    mt5bridge::ManagedAllocationCommitStatus commit(
        const mt5bridge::ManagedAllocationEntry &,
        const mt5bridge::ManagedBrokerVolumeMapping &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::SingleWriterLease &) override {
        return mt5bridge::ManagedAllocationCommitStatus::io_error;
    }

    mt5bridge::ManagedAllocationCommitStatus commit_staged(
        const mt5bridge::ManagedAllocationEntry &,
        const mt5bridge::ManagedBrokerVolumeMapping &,
        const mt5bridge::ManagedSettlementPreparation &,
        const mt5bridge::DurableManagedSettlementPreparationStore &,
        const mt5bridge::DurableJournalStore &,
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::SingleWriterLease &) override {
        return mt5bridge::ManagedAllocationCommitStatus::io_error;
    }

    mt5bridge::ManagedAllocationLoadResult load(
        const mt5bridge::ManagedAllocationKey &key) const override {
        const auto found = std::find_if(
            allocations.begin(), allocations.end(),
            [&key](const auto &candidate) { return candidate.allocation_key == key; });
        if (found == allocations.end())
            return {mt5bridge::ManagedAllocationLoadStatus::not_found, std::nullopt};
        return {mt5bridge::ManagedAllocationLoadStatus::found, *found};
    }

    mt5bridge::ManagedAllocationScanResult scan(
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &) const override {
        return scan_result();
    }

    mt5bridge::ManagedAllocationScanResult scan_recovery(
        const mt5bridge::DurableManagedDealSettlementStore &,
        const mt5bridge::DurableBrokerReversalStore &,
        const mt5bridge::DurableManagedOwnershipBasisStore &,
        const mt5bridge::DurableBrokerAllocationStore &,
        const mt5bridge::DurableManagedSettlementPreparationStore &) const override {
        return scan_result();
    }

    std::vector<mt5bridge::ManagedAllocationEntry> allocations;

private:
    mt5bridge::ManagedAllocationScanResult scan_result() const {
        mt5bridge::ManagedAllocationScanResult result;
        result.status = mt5bridge::ManagedAllocationScanStatus::complete;
        result.records = allocations;
        return result;
    }
};

class MemoryPreparationStore final
    : public mt5bridge::DurableManagedSettlementPreparationStore {
public:
    mt5bridge::ManagedSettlementPreparationLoadResult load(
        const mt5bridge::OperationKey &key, std::uint64_t revision,
        std::uint64_t deal_ticket) const override {
        const auto found = std::find_if(
            records.begin(), records.end(),
            [&key, revision, deal_ticket](const auto &candidate) {
                return candidate.target_operation.key == key &&
                       candidate.target_operation.revision == revision &&
                       candidate.fact.key.deal_ticket == deal_ticket;
            });
        if (found == records.end())
            return {mt5bridge::ManagedSettlementPreparationLoadStatus::not_found,
                    std::nullopt};
        return {mt5bridge::ManagedSettlementPreparationLoadStatus::found, *found};
    }

    std::vector<mt5bridge::ManagedSettlementPreparation> records;
};

mt5bridge::BrokerAllocationEnvelope envelope(
    const mt5bridge::BrokerReversalRecord &source) {
    mt5bridge::BrokerAllocationEnvelope result;
    result.account = source.account;
    result.broker_key = source.key();
    result.broker_provenance = source.provenance;
    result.broker_close_leg = source.broker_close_leg;
    result.broker_reverse_open_leg = source.broker_reverse_open_leg;
    result.unallocated_close_leg = source.broker_close_leg;
    result.unallocated_reverse_open_leg = source.broker_reverse_open_leg;
    require(result.valid(), "broker envelope fixture is invalid");
    return result;
}

mt5bridge::OperationRecord operation(
    const mt5bridge::ReconciliationBaseline &baseline, std::uint64_t revision,
    std::uint64_t settled_volume, std::uint64_t deal_ticket = 701,
    std::uint64_t operation_id = 4) {
    const auto key = mt5bridge::OperationKey{account(), 71, operation_id};
    mt5bridge::ReconciliationDescriptor descriptor{
        key.account,
        baseline,
        {mt5bridge::require_history_deal(deal_ticket,
                                         mt5bridge::ObservationWindow{1000, 2000})},
        mt5bridge::OperationState::partially_filled,
        key.trade_id,
        key.operation_id,
        5};

    mt5bridge::OperationRecord record;
    record.key = key;
    record.operation_kind = mt5bridge::OperationKind::close;
    record.request_payload = {0x01};
    record.result_payload = {0x02};
    record.operation_state = settled_volume == 0
                                 ? mt5bridge::OperationState::reconciling
                                 : mt5bridge::OperationState::partially_filled;
    record.journal_state = mt5bridge::JournalState::reconciling;
    record.revision = revision;
    record.fencing_token = 3;
    record.reconciliation_descriptor = std::move(descriptor);
    record.settled_volume = settled_volume;
    require(record.valid(), "operation fixture is invalid");
    return record;
}

void append_managed_provenance_fixture(
    std::vector<std::uint8_t> &bytes,
    const mt5bridge::ManagedSettlementProvenance &provenance) {
    mt5bridge::file_store_support::append_u64(bytes, provenance.graph_instance_id);
    mt5bridge::file_store_support::append_u64(bytes, provenance.graph_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.history_deals_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.evidence_digest);
}

void append_broker_provenance_fixture(
    std::vector<std::uint8_t> &bytes,
    const mt5bridge::BrokerReversalProvenance &provenance) {
    mt5bridge::file_store_support::append_u64(bytes, provenance.graph_instance_id);
    mt5bridge::file_store_support::append_u64(bytes, provenance.pre_graph_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.post_graph_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.pre_positions_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.post_positions_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.history_deals_revision);
    mt5bridge::file_store_support::append_u64(bytes, provenance.evidence_digest);
}

void append_volume_fixture(std::vector<std::uint8_t> &bytes,
                           const mt5bridge::BrokerVolume &volume) {
    mt5bridge::file_store_support::append_u64(bytes, volume.units);
    mt5bridge::file_store_support::append_u32(bytes, volume.scale);
    mt5bridge::file_store_support::append_u64(bytes, volume.step_units);
}

std::filesystem::path allocation_record_path_fixture(
    const std::filesystem::path &directory,
    const mt5bridge::ManagedAllocationKey &key) {
    const auto suffix =
        "alloc-" +
        mt5bridge::file_store_support::hex_u64(
            mt5bridge::file_store_support::fnv1a(key.broker_key.account.server)) +
        "-" + mt5bridge::file_store_support::hex_u64(key.broker_key.account.login) +
        "-" + mt5bridge::file_store_support::hex_u64(key.broker_key.deal_ticket) +
        "-" +
        mt5bridge::file_store_support::hex_u64(
            key.managed_key.operation_key.trade_id) +
        "-" +
        mt5bridge::file_store_support::hex_u64(
            key.managed_key.operation_key.operation_id) +
        "-" +
        mt5bridge::file_store_support::hex_u64(key.managed_key.deal_ticket) +
        "-" + mt5bridge::file_store_support::hex_u64(
                    static_cast<std::uint32_t>(key.leg)) +
        ".bin";
    return directory / suffix;
}

/// \brief Writes a v2 allocation fixture without invoking production INOUT admission.
/// \param directory Durable allocation directory.
/// \param record Valid allocation entry to encode.
///
/// The Stage B producer intentionally rejects INOUT at commit time. This test-only
/// fixture represents the immutable v2 record that a future authoritative producer
/// may publish, so restart validation can exercise the real file-store decoder and
/// source-validation path without weakening that production gate.
void write_v2_allocation_fixture(const std::filesystem::path &directory,
                                 const mt5bridge::ManagedAllocationEntry &record) {
    require(record.valid(), "allocation fixture is invalid");
    std::vector<std::uint8_t> body;
    const auto append_string = [&body](const std::string &value) {
        require(mt5bridge::file_store_support::append_string(body, value),
                "allocation fixture string is too long");
    };
    append_string(record.account.server);
    append_string(record.allocation_key.broker_key.account.server);
    append_string(record.allocation_key.managed_key.account.server);
    append_string(record.allocation_key.managed_key.operation_key.account.server);
    mt5bridge::file_store_support::append_u64(body, record.account.login);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.broker_key.account.login);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.broker_key.deal_ticket);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.managed_key.account.login);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.managed_key.operation_key.account.login);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.managed_key.operation_key.trade_id);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.managed_key.operation_key.operation_id);
    mt5bridge::file_store_support::append_u64(
        body, record.allocation_key.managed_key.deal_ticket);
    mt5bridge::file_store_support::append_u32(
        body, static_cast<std::uint32_t>(record.allocation_key.leg));
    append_managed_provenance_fixture(body, record.settlement_provenance);
    append_broker_provenance_fixture(body, record.broker_provenance);
    mt5bridge::file_store_support::append_u64(
        body, record.settlement_source_operation_revision);
    mt5bridge::file_store_support::append_u64(body, record.settlement_frontier_revision);
    mt5bridge::file_store_support::append_u64(
        body, record.settlement_frontier_digest.value_or(0));
    mt5bridge::file_store_support::append_u64(body, record.basis_operation_revision);
    mt5bridge::file_store_support::append_u64(body, record.managed_logical_units);
    append_volume_fixture(body, record.broker_volume);

    std::vector<std::uint8_t> bytes{'M', 'T', '5', 'A', 'L', 'G', '0', '1'};
    mt5bridge::file_store_support::append_u32(bytes, 2);
    mt5bridge::file_store_support::append_u64(bytes,
                                              static_cast<std::uint64_t>(body.size()));
    mt5bridge::file_store_support::append_u64(
        bytes, mt5bridge::file_store_support::checksum(body));
    bytes.insert(bytes.end(), body.begin(), body.end());

    std::filesystem::create_directories(directory);
    std::ofstream output(allocation_record_path_fixture(directory, record.allocation_key),
                         std::ios::binary | std::ios::trunc);
    require(output.good(), "allocation fixture could not be opened");
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "allocation fixture could not be written");
}

std::uint32_t read_u32_fixture(const std::vector<std::uint8_t> &bytes,
                               std::size_t offset) {
    require(offset + sizeof(std::uint32_t) <= bytes.size(),
            "preparation fixture u32 is truncated");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32_fixture(std::vector<std::uint8_t> &bytes, std::size_t offset,
                       std::uint32_t value) {
    require(offset + sizeof(std::uint32_t) <= bytes.size(),
            "preparation fixture u32 does not fit");
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
}

void write_u64_fixture(std::vector<std::uint8_t> &bytes, std::size_t offset,
                       std::uint64_t value) {
    require(offset + sizeof(std::uint64_t) <= bytes.size(),
            "preparation fixture checksum does not fit");
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
}

void skip_string_fixture(const std::vector<std::uint8_t> &bytes,
                         std::size_t &offset) {
    const auto size = read_u32_fixture(bytes, offset);
    offset += sizeof(std::uint32_t) + size;
    require(offset <= bytes.size(), "preparation fixture string is truncated");
}

void skip_payload_fixture(const std::vector<std::uint8_t> &bytes,
                          std::size_t &offset) {
    skip_string_fixture(bytes, offset);
}

void skip_u64_fixture(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                      std::size_t count) {
    require(count <= (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t) &&
                offset + count * sizeof(std::uint64_t) <= bytes.size(),
            "preparation fixture u64 fields are truncated");
    offset += count * sizeof(std::uint64_t);
}

std::size_t target_mode_offset(const std::vector<std::uint8_t> &bytes) {
    require(bytes.size() >= 28, "preparation fixture envelope is truncated");
    std::size_t offset = 28;
    skip_string_fixture(bytes, offset);
    skip_string_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 11); // fact identities, volume, provenance
    skip_string_fixture(bytes, offset);
    skip_string_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 10); // frontier identities and provenance
    const auto frontier_entries = read_u32_fixture(bytes, offset);
    offset += sizeof(std::uint32_t);
    skip_u64_fixture(bytes, offset, static_cast<std::size_t>(frontier_entries) * 3);
    skip_u64_fixture(bytes, offset, 1); // base operation revision
    skip_string_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 1); // staged account login
    skip_string_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 2); // broker account login and deal ticket
    skip_string_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 13); // staged operation, provenance, revisions
    skip_string_fixture(bytes, offset);
    skip_payload_fixture(bytes, offset);
    skip_payload_fixture(bytes, offset);
    skip_u64_fixture(bytes, offset, 3); // operation identity
    require(offset + sizeof(std::uint32_t) * 2 <= bytes.size(),
            "preparation operation mode is truncated");
    offset += sizeof(std::uint32_t); // operation kind
    return offset;
}

std::filesystem::path preparation_path_fixture(
    const std::filesystem::path &directory,
    const mt5bridge::ManagedSettlementPreparation &preparation) {
    const auto &key = preparation.frontier.operation_key;
    const auto name =
        "prepared-" +
        mt5bridge::file_store_support::hex_u64(
            mt5bridge::file_store_support::fnv1a(key.account.server)) +
        "-" + mt5bridge::file_store_support::hex_u64(key.account.login) +
        "-" + mt5bridge::file_store_support::hex_u64(key.trade_id) + "-" +
        mt5bridge::file_store_support::hex_u64(key.operation_id) + "-" +
        mt5bridge::file_store_support::hex_u64(preparation.frontier.operation_revision) +
        "-" + mt5bridge::file_store_support::hex_u64(preparation.fact.key.deal_ticket) +
        ".bin";
    return directory / name;
}

std::vector<std::uint8_t> read_fixture_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "preparation fixture could not be opened");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_fixture_bytes(const std::filesystem::path &path,
                         const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "preparation fixture could not be written");
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "preparation fixture write failed");
}

void rewrite_preparation_header(std::vector<std::uint8_t> &bytes,
                                std::uint32_t version) {
    require(bytes.size() >= 28, "preparation fixture header is truncated");
    const std::vector<std::uint8_t> body(bytes.begin() + 28, bytes.end());
    write_u32_fixture(bytes, 8, version);
    write_u64_fixture(bytes, 12, static_cast<std::uint64_t>(body.size()));
    write_u64_fixture(bytes, 20, mt5bridge::file_store_support::checksum(body));
}

std::vector<std::uint8_t> legacy_preparation_fixture(
    const std::vector<std::uint8_t> &v2_bytes) {
    const auto mode = target_mode_offset(v2_bytes);
    auto legacy = v2_bytes;
    legacy.erase(legacy.begin() + static_cast<std::ptrdiff_t>(mode),
                 legacy.begin() + static_cast<std::ptrdiff_t>(mode + 4));
    rewrite_preparation_header(legacy, 1);
    return legacy;
}

std::vector<std::uint8_t> corrupt_preparation_mode(
    const std::vector<std::uint8_t> &v2_bytes) {
    auto corrupt = v2_bytes;
    write_u32_fixture(corrupt, target_mode_offset(corrupt), 99);
    rewrite_preparation_header(corrupt, 2);
    return corrupt;
}

struct DurableSources {
    MemoryBrokerStore broker;
    MemoryEnvelopeStore envelope;
    mt5bridge::BrokerReversalRecord reversal = broker_record();

    DurableSources() {
        broker.records[reversal.key()] = reversal;
        envelope.records[reversal.key()] = ::envelope(reversal);
    }
};

struct ProofData {
    mt5bridge::ReconciliationBaseline baseline;
    mt5bridge::OperationRecord base_operation;
    mt5bridge::OperationRecord target_operation;
    mt5bridge::ManagedDealSettlementCommit proof;
};

struct PreparedFixture {
    mt5bridge_test_support::MemoryJournalStore journal;
    DurableSources sources;
    ProofData data;
    mt5bridge::ManagedStagedOwnershipAssociation staged;
    mt5bridge::ManagedBrokerVolumeMapping mapping;
    mt5bridge::ManagedAllocationEntry allocation;

    PreparedFixture()
        : sources(),
          data(make_proof_data()),
          staged{account(),
                 {sources.reversal.key(), data.target_operation.key},
                 sources.reversal.provenance,
                 8,
                 9,
                 0xfeed},
          mapping(*mt5bridge::derive_managed_broker_volume_mapping(
              data.proof.fact(), sources.reversal)),
          allocation(make_allocation(data.proof.fact(), data.proof.frontier(), mapping,
                                     sources.reversal)) {
        journal.records[data.base_operation.key] = data.base_operation;
        require(staged.valid(), "staged ownership fixture is invalid");
        require(mapping.valid(), "managed mapping fixture is invalid");
        require(allocation.valid(), "staged allocation fixture is invalid");
    }

private:
    static ProofData make_proof_data() {
        mt5bridge::ObservationGraph graph(account());
        mt5bridge::ObservationBatch batch;
        batch.account = account();
        batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        require(graph.apply(batch).accepted(), "baseline observation failed");
        const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);
        const auto base_operation = operation(baseline, 8, 0);
        const auto target_operation = operation(baseline, 9, 2);
        mt5bridge::ObservationBatch deal_batch;
        deal_batch.account = account();
        deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
        deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
        deal_batch.history_deals = {deal_snapshot()};
        require(graph.apply(deal_batch).accepted(), "proof deal observation failed");
        const auto proof = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
            target_operation, baseline, graph, 701, 2);
        require(proof.has_value(), "staged settlement proof was not derived");
        return {baseline, base_operation, target_operation, *proof};
    }

public:
    static mt5bridge::ManagedAllocationEntry make_allocation(
        const mt5bridge::ManagedDealSettlement &fact,
        const mt5bridge::ManagedSettlementFrontier &frontier,
        const mt5bridge::ManagedBrokerVolumeMapping &mapping,
        const mt5bridge::BrokerReversalRecord &reversal) {
        mt5bridge::ManagedAllocationEntry result;
        result.account = account();
        result.allocation_key = {reversal.key(), fact.key,
                                 mt5bridge::ManagedAllocationLeg::close};
        result.settlement_provenance = fact.provenance;
        result.broker_provenance = reversal.provenance;
        result.settlement_source_operation_revision = fact.source_operation_revision;
        result.settlement_frontier_revision = frontier.operation_revision;
        result.settlement_frontier_digest =
            mt5bridge::managed_settlement_frontier_digest(frontier);
        result.basis_operation_revision = frontier.operation_revision;
        result.managed_logical_units = fact.managed_logical_units;
        result.broker_volume = mapping.broker_volume();
        return result;
    }
};

void run_restart_regression() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_settlement_preparation_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::remove_all(root);
    const auto settlement_root = root / "settlement";
    const auto allocation_root = root / "allocation";
    const auto basis_root = root / "basis";
    const auto preparation_root = root / "preparation";

    {
        PreparedFixture late_partial_fixture;
        auto late_partial_base = late_partial_fixture.data.base_operation;
        late_partial_base.operation_state = mt5bridge::OperationState::partially_filled;
        late_partial_base.settled_volume = 1;
        require(late_partial_base.valid(), "late-partial base fixture is malformed");
        late_partial_fixture.journal.records[late_partial_base.key] = late_partial_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore late_partial_store(
            root / "late-partial");
        require(late_partial_store.prepare(
                    late_partial_fixture.data.proof,
                    late_partial_fixture.data.target_operation, 8,
                    late_partial_fixture.staged, late_partial_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "valid incremental partial settlement was rejected");
    }

    {
        PreparedFixture gated_fixture;
        gated_fixture.data.base_operation.settlement_mode =
            mt5bridge::ManagedSettlementMode::allocation_gated;
        gated_fixture.data.target_operation.settlement_mode =
            mt5bridge::ManagedSettlementMode::allocation_gated;
        require(gated_fixture.data.base_operation.valid() &&
                    gated_fixture.data.target_operation.valid(),
                "allocation-gated preparation fixture is malformed");
        gated_fixture.journal.records[gated_fixture.data.base_operation.key] =
            gated_fixture.data.base_operation;
        const auto gated_root = root / "gated-mode";
        mt5bridge::WindowsFileManagedSettlementPreparationStore gated_store(gated_root);
        require(gated_store.prepare(
                    gated_fixture.data.proof, gated_fixture.data.target_operation, 8,
                    gated_fixture.staged, gated_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "allocation-gated preparation was not published");
        const auto gated_scan = gated_store.scan();
        require(gated_scan.complete() && gated_scan.records.size() == 1 &&
                    gated_scan.records.front().target_operation.settlement_mode ==
                        mt5bridge::ManagedSettlementMode::allocation_gated,
                "allocation-gated preparation mode did not survive restart scan");
    }

    {
        PreparedFixture compatibility_fixture;
        const auto compatibility_root = root / "format-compatibility";
        mt5bridge::WindowsFileManagedSettlementPreparationStore writer(
            compatibility_root);
        require(writer.prepare(compatibility_fixture.data.proof,
                               compatibility_fixture.data.target_operation, 8,
                               compatibility_fixture.staged,
                               compatibility_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "compatibility preparation was not published");
        const auto path = preparation_path_fixture(
            compatibility_root,
            mt5bridge::ManagedSettlementPreparation{
                compatibility_fixture.data.proof.fact(),
                compatibility_fixture.data.proof.frontier(),
                compatibility_fixture.data.target_operation, 8,
                compatibility_fixture.staged});
        const auto v2_bytes = read_fixture_bytes(path);
        write_fixture_bytes(path, legacy_preparation_fixture(v2_bytes));
        mt5bridge::WindowsFileManagedSettlementPreparationStore legacy_reader(
            compatibility_root);
        const auto legacy_loaded = legacy_reader.load(
            compatibility_fixture.data.target_operation.key,
            compatibility_fixture.data.target_operation.revision,
            compatibility_fixture.data.proof.fact().key.deal_ticket);
        require(legacy_loaded.status ==
                    mt5bridge::ManagedSettlementPreparationLoadStatus::found &&
                    legacy_loaded.record &&
                    legacy_loaded.record->target_operation.settlement_mode ==
                        mt5bridge::ManagedSettlementMode::legacy,
                "real v1 preparation fixture was not decoded as legacy");
        const auto legacy_scan = legacy_reader.scan();
        require(legacy_scan.complete() && legacy_scan.records.size() == 1 &&
                    legacy_scan.records.front().target_operation.settlement_mode ==
                        mt5bridge::ManagedSettlementMode::legacy,
                "real v1 preparation fixture failed restart scan");

        write_fixture_bytes(path, corrupt_preparation_mode(v2_bytes));
        mt5bridge::WindowsFileManagedSettlementPreparationStore corrupt_reader(
            compatibility_root);
        require(corrupt_reader.load(
                    compatibility_fixture.data.target_operation.key,
                    compatibility_fixture.data.target_operation.revision,
                    compatibility_fixture.data.proof.fact().key.deal_ticket)
                        .status ==
                    mt5bridge::ManagedSettlementPreparationLoadStatus::invalid_record &&
                    corrupt_reader.scan().status ==
                        mt5bridge::ManagedSettlementPreparationScanStatus::invalid_record,
                "unknown preparation mode was not rejected fail-closed");
    }

    {
        PreparedFixture mismatch_fixture;
        auto mismatched_target = mismatch_fixture.data.target_operation;
        mismatched_target.settlement_mode =
            mt5bridge::ManagedSettlementMode::allocation_gated;
        require(mismatched_target.valid(), "mode mismatch target fixture is malformed");
        const auto mismatch_root = root / "mode-mismatch";
        mt5bridge::WindowsFileManagedSettlementPreparationStore mismatch_store(
            mismatch_root);
        require(mismatch_store.prepare(
                    mismatch_fixture.data.proof, mismatched_target, 8,
                    mismatch_fixture.staged, mismatch_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_operation_record,
                "preparation accepted a base/target mode mismatch");
        require(mismatch_store.scan().complete() && mismatch_store.scan().records.empty(),
                "mode mismatch wrote a preparation manifest");
    }

    {
        PreparedFixture forged_fixture;
        auto forged_target = forged_fixture.data.target_operation;
        forged_target.request_payload.push_back(0x7f);
        mt5bridge::WindowsFileManagedSettlementPreparationStore forged_store(
            root / "forged-immutable-target");
        require(forged_store.prepare(forged_fixture.data.proof, forged_target, 8,
                                     forged_fixture.staged, forged_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_operation_record,
                "preparation accepted a target with forged immutable payload");
    }

    {
        PreparedFixture invalid_transition_fixture;
        auto invalid_base = invalid_transition_fixture.data.base_operation;
        invalid_base.operation_state = mt5bridge::OperationState::accepted;
        require(invalid_base.valid(), "invalid-transition base fixture is malformed");
        invalid_transition_fixture.journal.records[invalid_base.key] = invalid_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore invalid_transition_store(
            root / "invalid-transition");
        require(invalid_transition_store.prepare(
                    invalid_transition_fixture.data.proof,
                    invalid_transition_fixture.data.target_operation, 8,
                    invalid_transition_fixture.staged, invalid_transition_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_operation_record,
                "preparation accepted an invalid lifecycle transition");
    }

    {
        PreparedFixture zero_requested_fixture;
        auto zero_requested_base = zero_requested_fixture.data.base_operation;
        auto zero_requested_target = zero_requested_fixture.data.target_operation;
        require(zero_requested_base.reconciliation_descriptor.has_value(),
                "zero-volume base fixture has no reconciliation descriptor");
        require(zero_requested_target.reconciliation_descriptor.has_value(),
                "zero-volume target fixture has no reconciliation descriptor");
        zero_requested_base.reconciliation_descriptor->requested_volume = 0;
        zero_requested_target.reconciliation_descriptor->requested_volume = 0;
        require(zero_requested_base.valid(), "zero-volume base fixture is malformed");
        require(zero_requested_target.valid(), "zero-volume target fixture is malformed");
        zero_requested_fixture.journal.records[zero_requested_base.key] = zero_requested_base;
        mt5bridge::WindowsFileManagedSettlementPreparationStore zero_requested_store(
            root / "zero-requested-volume");
        require(zero_requested_store.prepare(
                    zero_requested_fixture.data.proof, zero_requested_target, 8,
                    zero_requested_fixture.staged, zero_requested_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::invalid_record,
                "preparation admitted a zero requested volume");
    }

    mt5bridge::OperationRecord durable_base_operation;
    {
        PreparedFixture fixture;
        mt5bridge::WindowsFileManagedSettlementPreparationStore preparation_store(
            preparation_root);
        require(preparation_store.ready(), "preparation store did not open");
        require(preparation_store.prepare(
                    fixture.data.proof, fixture.data.target_operation, 8, fixture.staged,
                    fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "durable preparation did not publish");

        mt5bridge::WindowsFileManagedDealSettlementStore settlement_store(settlement_root);
        require(settlement_store.ready(), "settlement store did not open");
        require(settlement_store.load(fixture.data.proof.fact().key).status ==
                    mt5bridge::ManagedDealSettlementLoadStatus::not_found,
                "prepared fact was admitted before finalization");
        const auto replay_status = preparation_store.prepare(
                    fixture.data.proof, fixture.data.target_operation, 8, fixture.staged,
                    fixture.journal);
        if (replay_status !=
            mt5bridge::ManagedSettlementPreparationStatus::already_prepared) {
            const auto replay_scan = preparation_store.scan();
            require(replay_scan.complete() && replay_scan.records.size() == 1,
                    "preparation replay manifest could not be decoded");
            require(replay_scan.records.front() ==
                        mt5bridge::ManagedSettlementPreparation{
                            fixture.data.proof.fact(), fixture.data.proof.frontier(),
                            fixture.data.target_operation, 8, fixture.staged},
                    "preparation replay payload changed");
            throw std::runtime_error("preparation replay returned unexpected status");
        }
        durable_base_operation = fixture.data.base_operation;
    }

    mt5bridge_test_support::MemoryJournalStore restarted_journal;
    restarted_journal.records[durable_base_operation.key] = durable_base_operation;
    DurableSources restarted_sources;
    mt5bridge::WindowsFileManagedSettlementPreparationStore restarted_preparation(
        preparation_root);
    mt5bridge::WindowsFileManagedDealSettlementStore restarted_settlement(settlement_root);
    mt5bridge::WindowsFileManagedAllocationStore restarted_allocation(allocation_root);
    mt5bridge::WindowsFileManagedOwnershipBasisStore restarted_basis(basis_root);
    require(restarted_preparation.ready() && restarted_settlement.ready() &&
                restarted_allocation.ready() && restarted_basis.ready(),
            "restarted Stage B stores did not open");

    const auto pending = restarted_preparation.scan();
    require(pending.complete() && pending.records.size() == 1,
            "restart did not recover the exact preparation manifest");
    const auto &recovered_preparation = pending.records.front();
    require(recovered_preparation.target_operation.key == durable_base_operation.key &&
                recovered_preparation.target_operation.revision == 9 &&
                recovered_preparation.base_operation_revision == 8 &&
                recovered_preparation.fact.key.deal_ticket == 701,
            "restart manifest changed the staged recipe");

    const auto recovered_mapping = mt5bridge::derive_managed_broker_volume_mapping(
        recovered_preparation.fact, restarted_sources.reversal);
    require(recovered_mapping.has_value(),
            "restart could not reconstruct the managed broker mapping");
    const auto recovered_allocation = PreparedFixture::make_allocation(
        recovered_preparation.fact, recovered_preparation.frontier, *recovered_mapping,
        restarted_sources.reversal);

    const auto staged_admission = mt5bridge::admit_staged_managed_allocation(
        recovered_allocation, *recovered_mapping, recovered_preparation, restarted_preparation,
        restarted_journal, restarted_settlement, restarted_sources.broker, restarted_basis,
        restarted_sources.envelope, restarted_allocation);
    require(staged_admission.status ==
                mt5bridge::ManagedStagedAllocationAdmissionStatus::invalid_broker,
            "unsupported INOUT source was admitted without an authoritative close proof");
    auto mismatched_frontier = recovered_allocation;
    mismatched_frontier.settlement_frontier_digest =
        *mismatched_frontier.settlement_frontier_digest + 1;
    require(mt5bridge::admit_staged_managed_allocation(
                mismatched_frontier, *recovered_mapping, recovered_preparation,
                restarted_preparation, restarted_journal, restarted_settlement,
                restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, restarted_allocation)
                .status == mt5bridge::ManagedStagedAllocationAdmissionStatus::invalid_mapping,
            "staged admission accepted a mismatched frontier digest");

    TestLease no_lease;
    no_lease.owned_account = {"Other-Account", 7};
    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, no_lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::lease_not_held,
            "recovery mutated without an authoritative account lease");

    TestLease lease;
    auto forged_preparation = recovered_preparation;
    forged_preparation.target_operation.request_payload.push_back(0x7f);
    require(forged_preparation.valid(), "forged preparation fixture is malformed");
    require(restarted_preparation.finalize(
                forged_preparation, restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker, restarted_allocation,
                lease) == mt5bridge::ManagedSettlementFinalizeStatus::conflict,
            "finalize accepted a preparation that differs from the durable manifest");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "manifest mismatch changed the journal before rejection");

    mt5bridge::WindowsFileManagedSettlementPreparationStore missing_preparation(
        root / "missing-preparation");
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                missing_preparation, restarted_journal, restarted_settlement,
                restarted_sources.broker, restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "staged allocation published without a durable preparation manifest");
    require(restarted_allocation.load(recovered_allocation.allocation_key).status ==
                mt5bridge::ManagedAllocationLoadStatus::not_found,
            "manifest-less staged allocation consumed capacity");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "unsupported INOUT preparation passed finalization without an authoritative proof");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "pending allocation recovery advanced the journal early");
    require(mt5bridge::validate_managed_ownership_basis(
                recovered_preparation.staged_ownership.settled_basis(), restarted_journal,
                restarted_sources.envelope) ==
                mt5bridge::ManagedOwnershipBasisProofStatus::invalid_operation_record,
            "staged ownership passed ordinary validation before journal CAS");

    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "unsupported INOUT allocation was committed without an authoritative proof");
    require(restarted_allocation.commit_staged(
                recovered_allocation, *recovered_mapping, recovered_preparation,
                restarted_preparation,
                restarted_journal, restarted_settlement, restarted_sources.broker,
                restarted_basis, restarted_sources.envelope, lease) ==
                mt5bridge::ManagedAllocationCommitStatus::invalid_source,
            "unsupported INOUT allocation replay changed its fail-closed result");
    require(restarted_settlement.load(recovered_preparation.fact.key).status ==
                mt5bridge::ManagedDealSettlementLoadStatus::not_found,
            "allocation publication leaked an unfinalized settlement fact");

    require(restarted_preparation.recover_pending(
                restarted_journal, restarted_settlement, restarted_basis,
                restarted_sources.envelope, restarted_sources.broker,
                restarted_allocation, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "recovery advanced an unsupported INOUT preparation without an allocation proof");
    require(restarted_journal.records.at(durable_base_operation.key).revision == 8,
            "fail-closed recovery advanced the journal");

    {
        PreparedFixture historical_fixture;
        const auto historical_root = root / "historical-replay";
        mt5bridge::WindowsFileManagedSettlementPreparationStore historical_preparation_store(
            historical_root);
        require(historical_preparation_store.prepare(
                    historical_fixture.data.proof, historical_fixture.data.target_operation, 8,
                    historical_fixture.staged, historical_fixture.journal) ==
                    mt5bridge::ManagedSettlementPreparationStatus::prepared,
                "historical replay preparation did not publish");

        const mt5bridge::ManagedSettlementPreparation historical_preparation{
            historical_fixture.data.proof.fact(), historical_fixture.data.proof.frontier(),
            historical_fixture.data.target_operation, 8, historical_fixture.staged};
        MemorySettlementStore historical_settlement;
        historical_settlement.facts.push_back(historical_preparation.fact);
        historical_settlement.frontiers.push_back(historical_preparation.frontier);
        MemoryBasisStore historical_basis;
        historical_basis.bases.push_back(
            historical_preparation.staged_ownership.settled_basis());
        MemoryAllocationStore historical_allocation;
        historical_allocation.allocations.push_back(historical_fixture.allocation);
        mt5bridge_test_support::MemoryJournalStore historical_journal;
        historical_journal.records[historical_preparation.target_operation.key] =
            operation(historical_fixture.data.baseline, 10, 3);
        require(historical_journal.records.at(historical_preparation.target_operation.key)
                    .valid(),
                "historical replay journal fixture is malformed");

        require(historical_preparation_store.finalize(
                    historical_preparation, historical_journal, historical_settlement,
                    historical_basis, historical_fixture.sources.envelope,
                    historical_fixture.sources.broker, historical_allocation, lease) ==
                    mt5bridge::ManagedSettlementFinalizeStatus::already_finalized,
                "completed historical preparation was not skipped after journal advancement");
        require(historical_journal.records.at(historical_preparation.target_operation.key)
                        .revision == 10,
                "historical replay changed the newer journal revision");
    }

    std::filesystem::remove_all(root);
}

// This regression exercises cumulative frontier claims across two immutable
// facts. The current single-deal operation contract does not permit changing
// an operation's immutable deal predicate between revisions, so the A/B
// frontier is a test-only recovery model rather than a journal transition.
void run_cumulative_frontier_recovery_regression() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mt5bridge_managed_settlement_incremental_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    const auto preparation_root = root / "preparation";
    const auto allocation_root = root / "allocations";
    std::filesystem::remove_all(root);

    mt5bridge::ObservationGraph graph(account());
    mt5bridge::ObservationBatch baseline_batch;
    baseline_batch.account = account();
    baseline_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    baseline_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    require(graph.apply(baseline_batch).accepted(), "incremental baseline observation failed");
    const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);

    auto deal_a = deal_snapshot();
    deal_a.ticket = 702;
    deal_a.volume = 1.0;
    deal_a.time_msc = 1400;
    mt5bridge::ObservationBatch first_deal_batch;
    first_deal_batch.account = account();
    first_deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    first_deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    first_deal_batch.history_deals = {deal_a};
    require(graph.apply(first_deal_batch).accepted(), "first incremental deal observation failed");

    const auto target_a = operation(baseline, 9, 1, 702);
    const auto proof_a = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
        target_a, baseline, graph, 702, 1);
    require(proof_a.has_value(), "first incremental proof was not derived");
    const auto fact_a = proof_a->fact();
    const auto frontier_a = proof_a->frontier();

    auto deal_b = deal_snapshot();
    mt5bridge::ObservationBatch second_deal_batch;
    second_deal_batch.account = account();
    second_deal_batch.observed_domains = mt5bridge::ObservationDomain::history_deals;
    second_deal_batch.history_deals_window = mt5bridge::ObservationWindow{1000, 2000};
    second_deal_batch.history_deals = {deal_a, deal_b};
    require(graph.apply(second_deal_batch).accepted(), "second incremental deal observation failed");

    const auto base_b = operation(baseline, 9, 1, 701);
    const auto target_b = operation(baseline, 10, 3, 701);
    const auto proof_b = mt5bridge::dispatch::ManagedDealSettlementProducer::create(
        target_b, baseline, graph, 701, 2, {fact_a});
    require(proof_b.has_value(), "second incremental proof was not derived");
    const auto fact_b = proof_b->fact();
    const auto frontier_b = proof_b->frontier();
    require(frontier_b.entries.size() == 2 && frontier_b.settled_volume == 3,
            "second incremental frontier is not cumulative");

    const auto broker_a = broker_record_for(702, 303);
    const auto broker_b = broker_record();
    const auto mapping_a =
        mt5bridge::derive_managed_broker_volume_mapping(fact_a, broker_a);
    const auto mapping_b =
        mt5bridge::derive_managed_broker_volume_mapping(fact_b, broker_b);
    require(mapping_a && mapping_b, "incremental broker mappings were not derived");

    const auto allocation_a = PreparedFixture::make_allocation(
        fact_a, frontier_a, *mapping_a, broker_a);
    const auto allocation_b = PreparedFixture::make_allocation(
        fact_b, frontier_b, *mapping_b, broker_b);
    require(allocation_a.valid() && allocation_b.valid(),
            "incremental allocation fixtures are invalid");
    require(allocation_a.settlement_frontier_revision == frontier_a.operation_revision &&
                allocation_b.settlement_frontier_revision == frontier_b.operation_revision &&
                allocation_a.settlement_frontier_digest ==
                    mt5bridge::managed_settlement_frontier_digest(frontier_a),
            "immutable allocation frontier references were rewritten");

    mt5bridge::ManagedStagedOwnershipAssociation staged_b{
        account(), {broker_b.key(), target_b.key}, broker_b.provenance, 9, 10, 0xbeef};
    require(staged_b.valid(), "incremental staged ownership fixture is invalid");
    mt5bridge::ManagedStagedOwnershipAssociation staged_a{
        account(), {broker_a.key(), target_a.key}, broker_a.provenance, 8, 9, 0xcafe};
    require(staged_a.valid(), "first staged ownership fixture is invalid");
    mt5bridge::ManagedSettlementPreparation preparation_a{
        fact_a, frontier_a, target_a, 8, staged_a};
    mt5bridge::ManagedSettlementPreparation preparation_b{
        fact_b, frontier_b, target_b, 9, staged_b};
    require(preparation_a.valid(), "first preparation fixture is invalid");
    require(preparation_b.valid(), "incremental preparation fixture is invalid");

    mt5bridge_test_support::MemoryJournalStore preparation_journal;
    preparation_journal.records[base_b.key] = base_b;
    mt5bridge::WindowsFileManagedSettlementPreparationStore preparation_store(
        preparation_root);
    require(preparation_store.prepare(
                *proof_b, target_b, 9, staged_b, preparation_journal) ==
                mt5bridge::ManagedSettlementPreparationStatus::prepared,
            "incremental preparation did not publish");

    mt5bridge::WindowsFileManagedSettlementPreparationStore restarted_preparation(
        preparation_root);
    const auto scanned = restarted_preparation.scan();
    require(scanned.complete() && scanned.records.size() == 1 &&
                scanned.records.front() == preparation_b,
            "durable preparation did not survive restart");

    MemoryBrokerStore broker_store;
    broker_store.records[broker_a.key()] = broker_a;
    broker_store.records[broker_b.key()] = broker_b;
    MemoryEnvelopeStore envelope_store;
    envelope_store.records[broker_a.key()] = envelope(broker_a);
    envelope_store.records[broker_b.key()] = envelope(broker_b);

    MemorySettlementStore settlement_store;
    settlement_store.facts = {fact_a, fact_b};
    settlement_store.frontiers = {frontier_a, frontier_b};
    MemoryBasisStore basis_store;
    basis_store.bases = {staged_a.settled_basis(), staged_b.settled_basis()};

    // The A manifest is intentionally supplied through a test-only lookup seam:
    // the current production single-deal binding cannot prepare two conflicting
    // immutable descriptors under one journal key. The real allocation scanner
    // still validates both durable v2 records against these exact manifests.
    MemoryPreparationStore source_preparations;
    source_preparations.records = {preparation_a, preparation_b};

    // Production commit remains fail-closed for INOUT. Write only the exact
    // v2 bytes that a future authoritative producer would publish, then let
    // the real file-backed scan_recovery() validate every source.
    write_v2_allocation_fixture(allocation_root, allocation_a);
    write_v2_allocation_fixture(allocation_root, allocation_b);
    mt5bridge::WindowsFileManagedAllocationStore restarted_allocation(allocation_root);
    require(restarted_allocation.ready(), "cumulative allocation store did not open");
    const auto recovered_allocations = restarted_allocation.scan_recovery(
        settlement_store, broker_store, basis_store, envelope_store,
        source_preparations);
    require(recovered_allocations.complete() && recovered_allocations.records.size() == 2,
            "historical replay did not validate durable allocation sources");
    MemoryPreparationStore missing_preparation;
    missing_preparation.records = {preparation_b};
    const auto missing_source = restarted_allocation.scan_recovery(
        settlement_store, broker_store, basis_store, envelope_store,
        missing_preparation);
    require(!missing_source.complete(),
            "historical allocation without its preparation was admitted");

    MemoryAllocationStore allocation_store;
    allocation_store.allocations = {allocation_a, allocation_b};

    mt5bridge_test_support::MemoryJournalStore current_journal;
    current_journal.records[target_b.key] = operation(baseline, 11, 4, 701);
    TestLease lease;
    require(restarted_preparation.finalize(
                preparation_b, current_journal, settlement_store, basis_store,
                envelope_store, broker_store, allocation_store, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::already_finalized,
            "incremental historical replay rejected cumulative allocations");
    require(current_journal.records.at(target_b.key).revision == 11,
            "incremental historical replay changed the current journal");

    auto incomplete_allocation_store = allocation_store;
    incomplete_allocation_store.allocations.erase(
        std::remove_if(incomplete_allocation_store.allocations.begin(),
                       incomplete_allocation_store.allocations.end(),
                       [&allocation_a](const auto &candidate) {
                           return candidate.allocation_key == allocation_a.allocation_key;
                       }),
        incomplete_allocation_store.allocations.end());
    require(restarted_preparation.finalize(
                preparation_b, current_journal, settlement_store, basis_store,
                envelope_store, broker_store, incomplete_allocation_store, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "incomplete prior frontier allocation was admitted");

    auto forged_current = current_journal.records.at(target_b.key);
    forged_current.request_payload.push_back(0x7f);
    current_journal.records[target_b.key] = forged_current;
    require(restarted_preparation.finalize(
                preparation_b, current_journal, settlement_store, basis_store,
                envelope_store, broker_store, allocation_store, lease) ==
                mt5bridge::ManagedSettlementFinalizeStatus::invalid_source,
            "historical replay accepted a journal with forged immutable payload");

    std::filesystem::remove_all(root);
}

} // namespace

/// \brief Runs durable staged preparation and restart recovery checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        run_restart_regression();
        run_cumulative_frontier_recovery_regression();
        std::cout << "managed settlement preparation checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "managed settlement preparation checks failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}

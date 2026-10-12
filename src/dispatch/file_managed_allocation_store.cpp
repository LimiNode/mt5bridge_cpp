/// \file file_managed_allocation_store.cpp
/// \brief Implements the Windows-backed managed allocation ledger.

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>

#include "file_store_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'A', 'L', 'G', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes = 1024U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

using namespace file_store_support;

enum class SourceValidation { valid, missing, invalid, io_error };

void append_volume(std::vector<std::uint8_t> &bytes, const BrokerVolume &volume) {
    append_u64(bytes, volume.units);
    append_u32(bytes, volume.scale);
    append_u64(bytes, volume.step_units);
}

bool read_volume(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 BrokerVolume &volume) {
    return read_u64(bytes, offset, volume.units) &&
           read_u32(bytes, offset, volume.scale) &&
           read_u64(bytes, offset, volume.step_units);
}

void append_managed_provenance(std::vector<std::uint8_t> &bytes,
                               const ManagedSettlementProvenance &provenance) {
    append_u64(bytes, provenance.graph_instance_id);
    append_u64(bytes, provenance.graph_revision);
    append_u64(bytes, provenance.history_deals_revision);
    append_u64(bytes, provenance.evidence_digest);
}

bool read_managed_provenance(const std::vector<std::uint8_t> &bytes,
                             std::size_t &offset,
                             ManagedSettlementProvenance &provenance) {
    return read_u64(bytes, offset, provenance.graph_instance_id) &&
           read_u64(bytes, offset, provenance.graph_revision) &&
           read_u64(bytes, offset, provenance.history_deals_revision) &&
           read_u64(bytes, offset, provenance.evidence_digest);
}

void append_broker_provenance(std::vector<std::uint8_t> &bytes,
                              const BrokerReversalProvenance &provenance) {
    append_u64(bytes, provenance.graph_instance_id);
    append_u64(bytes, provenance.pre_graph_revision);
    append_u64(bytes, provenance.post_graph_revision);
    append_u64(bytes, provenance.pre_positions_revision);
    append_u64(bytes, provenance.post_positions_revision);
    append_u64(bytes, provenance.history_deals_revision);
    append_u64(bytes, provenance.evidence_digest);
}

bool read_broker_provenance(const std::vector<std::uint8_t> &bytes,
                            std::size_t &offset,
                            BrokerReversalProvenance &provenance) {
    return read_u64(bytes, offset, provenance.graph_instance_id) &&
           read_u64(bytes, offset, provenance.pre_graph_revision) &&
           read_u64(bytes, offset, provenance.post_graph_revision) &&
           read_u64(bytes, offset, provenance.pre_positions_revision) &&
           read_u64(bytes, offset, provenance.post_positions_revision) &&
           read_u64(bytes, offset, provenance.history_deals_revision) &&
           read_u64(bytes, offset, provenance.evidence_digest);
}

std::optional<std::vector<std::uint8_t>> serialize_body(
    const ManagedAllocationEntry &record) {
    if (!record.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_string(body, record.account.server) ||
        !append_string(body, record.allocation_key.broker_key.account.server) ||
        !append_string(body, record.allocation_key.managed_key.account.server) ||
        !append_string(body, record.allocation_key.managed_key.operation_key.account.server))
        return std::nullopt;
    append_u64(body, record.account.login);
    append_u64(body, record.allocation_key.broker_key.account.login);
    append_u64(body, record.allocation_key.broker_key.deal_ticket);
    append_u64(body, record.allocation_key.managed_key.account.login);
    append_u64(body, record.allocation_key.managed_key.operation_key.account.login);
    append_u64(body, record.allocation_key.managed_key.operation_key.trade_id);
    append_u64(body, record.allocation_key.managed_key.operation_key.operation_id);
    append_u64(body, record.allocation_key.managed_key.deal_ticket);
    append_u32(body, static_cast<std::uint32_t>(record.allocation_key.leg));
    append_managed_provenance(body, record.settlement_provenance);
    append_broker_provenance(body, record.broker_provenance);
    append_u64(body, record.settlement_source_operation_revision);
    append_u64(body, record.basis_operation_revision);
    append_u64(body, record.managed_logical_units);
    append_volume(body, record.broker_volume);
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> serialize_record(
    const ManagedAllocationEntry &record) {
    const auto body = serialize_body(record);
    if (!body)
        return std::nullopt;
    std::vector<std::uint8_t> result;
    result.reserve(kEnvelopeBytes + body->size());
    result.insert(result.end(), kMagic.begin(), kMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body->size()));
    append_u64(result, checksum(*body));
    result.insert(result.end(), body->begin(), body->end());
    return result;
}

std::optional<ManagedAllocationEntry> deserialize_record(
    const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < kEnvelopeBytes || bytes.size() > kMaxRecordBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return std::nullopt;
    std::size_t offset = kMagic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, offset, version) || !read_u64(bytes, offset, body_size) ||
        !read_u64(bytes, offset, expected_checksum) || version != kFormatVersion ||
        body_size > kMaxBodyBytes || body_size != bytes.size() - offset)
        return std::nullopt;
    const std::vector<std::uint8_t> body(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    if (checksum(body) != expected_checksum)
        return std::nullopt;

    ManagedAllocationEntry record;
    std::size_t body_offset = 0;
    std::uint32_t leg = 0;
    if (!read_string(body, body_offset, record.account.server) ||
        !read_string(body, body_offset, record.allocation_key.broker_key.account.server) ||
        !read_string(body, body_offset, record.allocation_key.managed_key.account.server) ||
        !read_string(body, body_offset,
                     record.allocation_key.managed_key.operation_key.account.server) ||
        !read_u64(body, body_offset, record.account.login) ||
        !read_u64(body, body_offset, record.allocation_key.broker_key.account.login) ||
        !read_u64(body, body_offset, record.allocation_key.broker_key.deal_ticket) ||
        !read_u64(body, body_offset, record.allocation_key.managed_key.account.login) ||
        !read_u64(body, body_offset,
                  record.allocation_key.managed_key.operation_key.account.login) ||
        !read_u64(body, body_offset,
                  record.allocation_key.managed_key.operation_key.trade_id) ||
        !read_u64(body, body_offset,
                  record.allocation_key.managed_key.operation_key.operation_id) ||
        !read_u64(body, body_offset, record.allocation_key.managed_key.deal_ticket) ||
        !read_u32(body, body_offset, leg) ||
        !read_managed_provenance(body, body_offset, record.settlement_provenance) ||
        !read_broker_provenance(body, body_offset, record.broker_provenance) ||
        !read_u64(body, body_offset, record.settlement_source_operation_revision) ||
        !read_u64(body, body_offset, record.basis_operation_revision) ||
        !read_u64(body, body_offset, record.managed_logical_units) ||
        !read_volume(body, body_offset, record.broker_volume) ||
        body_offset != body.size())
        return std::nullopt;
    record.allocation_key.leg = static_cast<ManagedAllocationLeg>(leg);
    return record.valid() ? std::optional<ManagedAllocationEntry>(std::move(record))
                          : std::nullopt;
}

#if defined(_WIN32)

ReadStatus read_record_file(const std::filesystem::path &path,
                            std::optional<ManagedAllocationEntry> &record,
                            std::string &error) {
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, kMaxRecordBytes, bytes, error,
                                        ReadStatus::malformed);
    if (status != ReadStatus::valid)
        return status;
    record = deserialize_record(bytes);
    return record ? ReadStatus::valid : ReadStatus::malformed;
}

#endif

std::filesystem::path record_path(const std::filesystem::path &directory,
                                  const ManagedAllocationKey &key) {
    const std::string suffix =
        "alloc-" + hex_u64(fnv1a(key.broker_key.account.server)) + "-" +
        hex_u64(key.broker_key.account.login) + "-" +
        hex_u64(key.broker_key.deal_ticket) + "-" +
        hex_u64(key.managed_key.operation_key.trade_id) + "-" +
        hex_u64(key.managed_key.operation_key.operation_id) + "-" +
        hex_u64(key.managed_key.deal_ticket) + "-" +
        hex_u64(static_cast<std::uint32_t>(key.leg)) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

#if defined(_WIN32)

ManagedAllocationScanStatus collect_records_locked(
    const std::filesystem::path &directory,
    std::vector<ManagedAllocationEntry> &records, std::string &error) {
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" ||
                filename.rfind(L"alloc-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                error = "managed allocation entry is not a regular file";
                return ManagedAllocationScanStatus::invalid_record;
            }
            std::optional<ManagedAllocationEntry> record;
            const auto status = read_record_file(entry.path(), record, error);
            if (status == ReadStatus::io_error)
                return ManagedAllocationScanStatus::io_error;
            if (status != ReadStatus::valid || !record ||
                record_path(directory, record->allocation_key).filename() !=
                    entry.path().filename()) {
                if (error.empty())
                    error = "malformed or mismatched managed allocation entry";
                return ManagedAllocationScanStatus::invalid_record;
            }
            records.push_back(std::move(*record));
        }
    } catch (const std::filesystem::filesystem_error &filesystem_error) {
        error = filesystem_error.what();
        return ManagedAllocationScanStatus::io_error;
    }
    std::sort(records.begin(), records.end(),
              [](const auto &left, const auto &right) {
                  return left.allocation_key < right.allocation_key;
              });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].allocation_key == records[index].allocation_key) {
            error = "duplicate managed allocation key";
            return ManagedAllocationScanStatus::invalid_record;
        }
    }
    return ManagedAllocationScanStatus::complete;
}

SourceValidation validate_sources(
    const std::vector<ManagedAllocationEntry> &records,
    const DurableManagedDealSettlementStore &settlement_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store,
    std::string &error) {
    std::set<ManagedDealSettlementKey> managed_keys;
    std::map<BrokerReversalKey, BrokerVolume> totals;
    std::map<BrokerReversalKey, BrokerVolume> capacities;
    for (const auto &entry : records) {
        if (!managed_keys.insert(entry.allocation_key.managed_key).second) {
            error = "duplicate managed settlement allocation";
            return SourceValidation::invalid;
        }

        const auto settlement = settlement_store.load(entry.allocation_key.managed_key);
        if (settlement.status == ManagedDealSettlementLoadStatus::not_found)
            return SourceValidation::missing;
        if (settlement.status == ManagedDealSettlementLoadStatus::io_error)
            return SourceValidation::io_error;
        if (settlement.status != ManagedDealSettlementLoadStatus::found ||
            !settlement.record || !settlement.record->valid()) {
            error = "managed allocation settlement source is invalid";
            return SourceValidation::invalid;
        }
        if (!(settlement.record->key == entry.allocation_key.managed_key) ||
            !(settlement.record->provenance == entry.settlement_provenance) ||
            settlement.record->source_operation_revision !=
                entry.settlement_source_operation_revision ||
            settlement.record->managed_logical_units != entry.managed_logical_units) {
            error = "managed allocation settlement source mismatch";
            return SourceValidation::invalid;
        }

        const auto broker = broker_store.load(entry.allocation_key.broker_key);
        if (broker.status == BrokerReversalLoadStatus::not_found)
            return SourceValidation::missing;
        if (broker.status == BrokerReversalLoadStatus::io_error)
            return SourceValidation::io_error;
        if (broker.status != BrokerReversalLoadStatus::found || !broker.record ||
            !broker.record->valid()) {
            error = "managed allocation broker source is invalid";
            return SourceValidation::invalid;
        }
        if (!(broker.record->key() == entry.allocation_key.broker_key) ||
            !(broker.record->provenance == entry.broker_provenance)) {
            error = "managed allocation broker source mismatch";
            return SourceValidation::invalid;
        }

        const auto basis_key = ManagedOwnershipBasisKey{
            entry.allocation_key.broker_key, entry.allocation_key.managed_key.operation_key};
        const auto basis = basis_store.load(basis_key);
        if (basis.status == ManagedOwnershipBasisLoadStatus::not_found)
            return SourceValidation::missing;
        if (basis.status == ManagedOwnershipBasisLoadStatus::io_error)
            return SourceValidation::io_error;
        if (basis.status != ManagedOwnershipBasisLoadStatus::found || !basis.record ||
            !basis.record->valid()) {
            error = "managed allocation ownership basis is invalid";
            return SourceValidation::invalid;
        }
        if (basis.record->account != entry.account || !(basis.record->key() == basis_key) ||
            !(basis.record->broker_provenance == entry.broker_provenance) ||
            basis.record->operation_revision != entry.basis_operation_revision ||
            entry.settlement_source_operation_revision > basis.record->operation_revision) {
            error = "managed allocation ownership basis mismatch";
            return SourceValidation::invalid;
        }

        const auto envelope = envelope_store.load(entry.allocation_key.broker_key);
        if (envelope.status == BrokerAllocationLoadStatus::not_found)
            return SourceValidation::missing;
        if (envelope.status == BrokerAllocationLoadStatus::io_error)
            return SourceValidation::io_error;
        if (envelope.status != BrokerAllocationLoadStatus::found || !envelope.record ||
            !envelope.record->valid()) {
            error = "managed allocation envelope is invalid";
            return SourceValidation::invalid;
        }
        if (envelope.record->account != entry.account ||
            !(envelope.record->key() == entry.allocation_key.broker_key) ||
            !(envelope.record->broker_provenance == entry.broker_provenance) ||
            !(envelope.record->broker_close_leg.scale == entry.broker_volume.scale) ||
            !(envelope.record->broker_close_leg.step_units == entry.broker_volume.step_units) ||
            !(envelope.record->unallocated_close_leg == envelope.record->broker_close_leg) ||
            !(envelope.record->unallocated_reverse_open_leg ==
              envelope.record->broker_reverse_open_leg)) {
            error = "managed allocation envelope mismatch";
            return SourceValidation::invalid;
        }

        const auto derived = derive_managed_broker_volume_mapping(
            *settlement.record, *broker.record);
        if (!derived || !(derived->managed_key() == entry.allocation_key.managed_key) ||
            !(derived->broker_key() == entry.allocation_key.broker_key) ||
            derived->managed_logical_units() != entry.managed_logical_units ||
            !(derived->managed_provenance() == entry.settlement_provenance) ||
            !(derived->broker_provenance() == entry.broker_provenance) ||
            !(derived->broker_volume() == entry.broker_volume)) {
            error = "managed allocation mapping mismatch";
            return SourceValidation::invalid;
        }

        const auto capacity = capacities.emplace(entry.allocation_key.broker_key,
                                                 envelope.record->broker_close_leg);
        if (!capacity.second && !(capacity.first->second == envelope.record->broker_close_leg)) {
            error = "managed allocation capacity changed across entries";
            return SourceValidation::invalid;
        }
        auto total = totals.find(entry.allocation_key.broker_key);
        if (total == totals.end()) {
            totals.emplace(entry.allocation_key.broker_key, entry.broker_volume);
        } else {
            if (total->second.scale != entry.broker_volume.scale ||
                total->second.step_units != entry.broker_volume.step_units ||
                total->second.units >
                    (std::numeric_limits<std::uint64_t>::max)() -
                        entry.broker_volume.units) {
                error = "managed allocation volume sum is invalid";
                return SourceValidation::invalid;
            }
            total->second.units += entry.broker_volume.units;
        }
        const auto &sum = totals.find(entry.allocation_key.broker_key)->second;
        if (sum.units > envelope.record->broker_close_leg.units) {
            error = "managed allocation close capacity is exhausted";
            return SourceValidation::invalid;
        }
    }
    return SourceValidation::valid;
}

ManagedAllocationCommitStatus source_status(SourceValidation status) {
    switch (status) {
    case SourceValidation::valid:
        return ManagedAllocationCommitStatus::committed;
    case SourceValidation::missing:
        return ManagedAllocationCommitStatus::missing_source;
    case SourceValidation::invalid:
        return ManagedAllocationCommitStatus::invalid_source;
    case SourceValidation::io_error:
        return ManagedAllocationCommitStatus::io_error;
    }
    return ManagedAllocationCommitStatus::io_error;
}

bool mapping_matches_entry(const ManagedAllocationEntry &entry,
                           const ManagedBrokerVolumeMapping &mapping) {
    return mapping.valid() && entry.account == mapping.managed_key().account &&
           entry.allocation_key.broker_key == mapping.broker_key() &&
           entry.allocation_key.managed_key == mapping.managed_key() &&
           entry.settlement_provenance == mapping.managed_provenance() &&
           entry.broker_provenance == mapping.broker_provenance() &&
           entry.managed_logical_units == mapping.managed_logical_units() &&
           entry.broker_volume == mapping.broker_volume();
}

#endif

} // namespace

WindowsFileManagedAllocationStore::WindowsFileManagedAllocationStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "managed allocation directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "managed allocation path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    (void)directory_;
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
#endif
}

ManagedAllocationCommitStatus WindowsFileManagedAllocationStore::commit(
    const ManagedAllocationEntry &entry, const ManagedBrokerVolumeMapping &mapping,
    const DurableJournalStore &journal_store,
    const DurableManagedDealSettlementStore &settlement_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store,
    const SingleWriterLease &lease) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed allocation store is unavailable");
        return ManagedAllocationCommitStatus::io_error;
    }
    if (!entry.valid() || !mapping_matches_entry(entry, mapping)) {
        set_error(last_error_, "invalid managed allocation entry or mapping");
        return ManagedAllocationCommitStatus::invalid_record;
    }
    const auto initial_token = lease.held_fencing_token(entry.account);
    if (!initial_token || *initial_token == 0) {
        set_error(last_error_, "managed allocation writer lease is not held");
        return ManagedAllocationCommitStatus::lease_not_held;
    }
    FileLock lock(directory_ / L".managed-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return ManagedAllocationCommitStatus::io_error;
    }
    const auto locked_token = lease.held_fencing_token(entry.account);
    if (!locked_token || *locked_token != *initial_token) {
        set_error(last_error_, "managed allocation writer lease changed");
        return ManagedAllocationCommitStatus::lease_not_held;
    }

    std::vector<ManagedAllocationEntry> records;
    std::string error;
    const auto collected = collect_records_locked(directory_, records, error);
    if (collected != ManagedAllocationScanStatus::complete) {
        set_error(last_error_, error.empty() ? "managed allocation scan failed" : error);
        return collected == ManagedAllocationScanStatus::io_error
                   ? ManagedAllocationCommitStatus::io_error
                   : ManagedAllocationCommitStatus::invalid_record;
    }
    const auto historical = validate_sources(
        records, settlement_store, broker_store, basis_store, envelope_store, error);
    if (historical != SourceValidation::valid) {
        set_error(last_error_, error.empty() ? "managed allocation source validation failed"
                                              : error);
        return source_status(historical);
    }
    const auto existing = std::find_if(
        records.begin(), records.end(), [&entry](const auto &candidate) {
            return candidate.allocation_key == entry.allocation_key;
        });
    if (existing != records.end()) {
        const auto final_token = lease.held_fencing_token(entry.account);
        if (!final_token || *final_token != *initial_token) {
            set_error(last_error_, "managed allocation writer lease changed");
            return ManagedAllocationCommitStatus::lease_not_held;
        }
        if (*existing == entry)
            return ManagedAllocationCommitStatus::already_committed;
        return ManagedAllocationCommitStatus::conflict;
    }

    const auto admission = admit_managed_broker_volume_mapping(
        mapping, journal_store, settlement_store, broker_store, basis_store,
        envelope_store);
    if (!admission.admitted() || !admission.mapping || !(*admission.mapping == mapping)) {
        if (admission.status == ManagedBrokerVolumeAdmissionStatus::missing_settlement_fact ||
            admission.status == ManagedBrokerVolumeAdmissionStatus::missing_broker_record ||
            admission.status == ManagedBrokerVolumeAdmissionStatus::missing_operation_record ||
            admission.status == ManagedBrokerVolumeAdmissionStatus::missing_ownership_basis ||
            admission.status == ManagedBrokerVolumeAdmissionStatus::missing_allocation_envelope)
            return ManagedAllocationCommitStatus::missing_source;
        if (admission.status == ManagedBrokerVolumeAdmissionStatus::io_error)
            return ManagedAllocationCommitStatus::io_error;
        set_error(last_error_, "managed allocation source admission failed");
        return ManagedAllocationCommitStatus::invalid_source;
    }

    const auto settlement = settlement_store.load(entry.allocation_key.managed_key);
    const auto basis = basis_store.load(ManagedOwnershipBasisKey{
        entry.allocation_key.broker_key, entry.allocation_key.managed_key.operation_key});
    if (settlement.status != ManagedDealSettlementLoadStatus::found || !settlement.record ||
        basis.status != ManagedOwnershipBasisLoadStatus::found || !basis.record ||
        settlement.record->source_operation_revision !=
            entry.settlement_source_operation_revision ||
        basis.record->operation_revision != entry.basis_operation_revision) {
        set_error(last_error_, "managed allocation revision evidence mismatch");
        return ManagedAllocationCommitStatus::invalid_source;
    }

    records.push_back(entry);
    error.clear();
    const auto complete = validate_sources(
        records, settlement_store, broker_store, basis_store, envelope_store, error);
    if (complete != SourceValidation::valid) {
        set_error(last_error_, error.empty() ? "managed allocation capacity validation failed"
                                              : error);
        return complete == SourceValidation::io_error
                   ? ManagedAllocationCommitStatus::io_error
                   : complete == SourceValidation::missing
                         ? ManagedAllocationCommitStatus::missing_source
                         : complete == SourceValidation::invalid &&
                                   error.find("capacity") != std::string::npos
                               ? ManagedAllocationCommitStatus::capacity_exhausted
                               : ManagedAllocationCommitStatus::invalid_source;
    }

    const auto final_token = lease.held_fencing_token(entry.account);
    if (!final_token || *final_token != *initial_token) {
        set_error(last_error_, "managed allocation writer lease changed");
        return ManagedAllocationCommitStatus::lease_not_held;
    }
    const auto bytes = serialize_record(entry);
    if (!bytes) {
        set_error(last_error_, "managed allocation entry exceeds storage limits");
        return ManagedAllocationCommitStatus::invalid_record;
    }
    const auto target = record_path(directory_, entry.allocation_key);
    const std::filesystem::path temporary(target.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, last_error_))
        return ManagedAllocationCommitStatus::io_error;
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = win32_error("MoveFileExW(managed allocation)");
        DeleteFileW(temporary.c_str());
        return ManagedAllocationCommitStatus::io_error;
    }
    return ManagedAllocationCommitStatus::committed;
#else
    (void)entry;
    (void)mapping;
    (void)journal_store;
    (void)settlement_store;
    (void)broker_store;
    (void)basis_store;
    (void)envelope_store;
    (void)lease;
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
    return ManagedAllocationCommitStatus::io_error;
#endif
}

ManagedAllocationLoadResult WindowsFileManagedAllocationStore::load(
    const ManagedAllocationKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed allocation store is unavailable");
        return {ManagedAllocationLoadStatus::io_error, std::nullopt};
    }
    if (!key.valid())
        return {ManagedAllocationLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L".managed-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedAllocationLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedAllocationEntry> record;
    std::string error;
    const auto status = read_record_file(record_path(directory_, key), record, error);
    if (status == ReadStatus::valid && record && record->allocation_key == key)
        return {ManagedAllocationLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed managed allocation entry" : error);
        return {ManagedAllocationLoadStatus::invalid_record, std::nullopt};
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {ManagedAllocationLoadStatus::io_error, std::nullopt};
    }
    if (status == ReadStatus::valid) {
        set_error(last_error_, "managed allocation filename collision");
        return {ManagedAllocationLoadStatus::invalid_record, std::nullopt};
    }
    return {ManagedAllocationLoadStatus::not_found, std::nullopt};
#else
    (void)key;
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
    return {ManagedAllocationLoadStatus::io_error, std::nullopt};
#endif
}

ManagedAllocationScanResult WindowsFileManagedAllocationStore::scan(
    const DurableManagedDealSettlementStore &settlement_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed allocation store is unavailable");
        return {ManagedAllocationScanStatus::io_error, {}};
    }
    FileLock lock(directory_ / L".managed-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedAllocationScanStatus::io_error, {}};
    }
    std::vector<ManagedAllocationEntry> records;
    std::string error;
    const auto collected = collect_records_locked(directory_, records, error);
    if (collected != ManagedAllocationScanStatus::complete) {
        set_error(last_error_, error.empty() ? "managed allocation scan failed" : error);
        return {collected, {}};
    }
    const auto status = validate_sources(
        records, settlement_store, broker_store, basis_store, envelope_store, error);
    if (status != SourceValidation::valid) {
        set_error(last_error_, error.empty() ? "managed allocation source validation failed"
                                              : error);
        return {status == SourceValidation::io_error
                    ? ManagedAllocationScanStatus::io_error
                    : ManagedAllocationScanStatus::invalid_record,
                {}};
    }
    return {ManagedAllocationScanStatus::complete, std::move(records)};
#else
    (void)settlement_store;
    (void)broker_store;
    (void)basis_store;
    (void)envelope_store;
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
    return {ManagedAllocationScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

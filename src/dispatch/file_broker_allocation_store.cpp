/// \file file_broker_allocation_store.cpp
/// \brief Implements the Windows-backed broker-allocation envelope store.

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <mt5bridge/dispatch/file_broker_allocation_store.hpp>

#include "file_store_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'E', 'N', 'V', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes = 768U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

using namespace file_store_support;

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

void append_provenance(std::vector<std::uint8_t> &bytes,
                       const BrokerReversalProvenance &provenance) {
    append_u64(bytes, provenance.graph_instance_id);
    append_u64(bytes, provenance.pre_graph_revision);
    append_u64(bytes, provenance.post_graph_revision);
    append_u64(bytes, provenance.pre_positions_revision);
    append_u64(bytes, provenance.post_positions_revision);
    append_u64(bytes, provenance.history_deals_revision);
    append_u64(bytes, provenance.evidence_digest);
}

bool read_provenance(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
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
    const BrokerAllocationEnvelope &record) {
    if (!record.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(256 + record.account.server.size());
    if (!append_string(body, record.account.server) ||
        !append_string(body, record.broker_key.account.server))
        return std::nullopt;
    append_u64(body, record.account.login);
    append_u64(body, record.broker_key.account.login);
    append_u64(body, record.broker_key.deal_ticket);
    append_provenance(body, record.broker_provenance);
    append_volume(body, record.broker_close_leg);
    append_volume(body, record.broker_reverse_open_leg);
    append_volume(body, record.unallocated_close_leg);
    append_volume(body, record.unallocated_reverse_open_leg);
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> serialize_record(
    const BrokerAllocationEnvelope &record) {
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

std::optional<BrokerAllocationEnvelope> deserialize_record(
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

    BrokerAllocationEnvelope record;
    std::size_t body_offset = 0;
    if (!read_string(body, body_offset, record.account.server) ||
        !read_string(body, body_offset, record.broker_key.account.server) ||
        !read_u64(body, body_offset, record.account.login) ||
        !read_u64(body, body_offset, record.broker_key.account.login) ||
        !read_u64(body, body_offset, record.broker_key.deal_ticket) ||
        !read_provenance(body, body_offset, record.broker_provenance) ||
        !read_volume(body, body_offset, record.broker_close_leg) ||
        !read_volume(body, body_offset, record.broker_reverse_open_leg) ||
        !read_volume(body, body_offset, record.unallocated_close_leg) ||
        !read_volume(body, body_offset, record.unallocated_reverse_open_leg) ||
        body_offset != body.size())
        return std::nullopt;
    return record.valid() ? std::optional<BrokerAllocationEnvelope>(std::move(record))
                          : std::nullopt;
}

#if defined(_WIN32)

ReadStatus read_record_file(const std::filesystem::path &path,
                            std::optional<BrokerAllocationEnvelope> &record,
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
                                  const BrokerReversalKey &key) {
    const std::string suffix = "env-" + hex_u64(fnv1a(key.account.server)) + "-" +
                               hex_u64(key.account.login) + "-" +
                               hex_u64(key.deal_ticket) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

} // namespace

WindowsFileBrokerAllocationStore::WindowsFileBrokerAllocationStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "broker allocation directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "broker allocation path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    (void)directory_;
    set_error(last_error_, "WindowsFileBrokerAllocationStore requires Windows");
#endif
}

BrokerAllocationCommitStatus WindowsFileBrokerAllocationStore::commit(
    const BrokerAllocationEnvelope &record,
    const DurableBrokerReversalStore &broker_store) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "broker allocation store is unavailable");
        return BrokerAllocationCommitStatus::io_error;
    }
    const auto proof = validate_broker_allocation_envelope(record, broker_store);
    if (proof != BrokerAllocationProofStatus::valid) {
        switch (proof) {
        case BrokerAllocationProofStatus::invalid_record:
            set_error(last_error_, "invalid broker allocation envelope");
            return BrokerAllocationCommitStatus::invalid_record;
        case BrokerAllocationProofStatus::missing_broker_record:
            set_error(last_error_, "durable broker reversal record is missing");
            return BrokerAllocationCommitStatus::missing_broker_record;
        case BrokerAllocationProofStatus::invalid_broker_record:
            set_error(last_error_, "durable broker reversal proof does not match");
            return BrokerAllocationCommitStatus::invalid_broker_record;
        case BrokerAllocationProofStatus::io_error:
            set_error(last_error_, "could not load durable broker reversal proof");
            return BrokerAllocationCommitStatus::io_error;
        case BrokerAllocationProofStatus::valid:
            break;
        }
    }
    const auto bytes = serialize_record(record);
    if (!bytes) {
        set_error(last_error_, "broker allocation envelope exceeds storage limits");
        return BrokerAllocationCommitStatus::invalid_record;
    }
    FileLock lock(directory_ / L".broker-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return BrokerAllocationCommitStatus::io_error;
    }
    const auto target = record_path(directory_, record.key());
    std::optional<BrokerAllocationEnvelope> existing;
    std::string read_error;
    const auto status = read_record_file(target, existing, read_error);
    if (status == ReadStatus::malformed) {
        set_error(last_error_, "malformed broker allocation envelope");
        return BrokerAllocationCommitStatus::invalid_record;
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return BrokerAllocationCommitStatus::io_error;
    }
    if (status == ReadStatus::valid && existing) {
        if (*existing == record)
            return BrokerAllocationCommitStatus::already_committed;
        return BrokerAllocationCommitStatus::conflict;
    }
    const std::filesystem::path temporary(target.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, last_error_))
        return BrokerAllocationCommitStatus::io_error;
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = win32_error("MoveFileExW(broker allocation)");
        DeleteFileW(temporary.c_str());
        return BrokerAllocationCommitStatus::io_error;
    }
    return BrokerAllocationCommitStatus::committed;
#else
    (void)record;
    (void)broker_store;
    set_error(last_error_, "WindowsFileBrokerAllocationStore requires Windows");
    return BrokerAllocationCommitStatus::io_error;
#endif
}

BrokerAllocationLoadResult WindowsFileBrokerAllocationStore::load(
    const BrokerReversalKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "broker allocation store is unavailable");
        return {BrokerAllocationLoadStatus::io_error, std::nullopt};
    }
    if (!key.valid())
        return {BrokerAllocationLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L".broker-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {BrokerAllocationLoadStatus::io_error, std::nullopt};
    }
    std::optional<BrokerAllocationEnvelope> record;
    std::string error;
    const auto status = read_record_file(record_path(directory_, key), record, error);
    if (status == ReadStatus::valid && record && record->key() == key)
        return {BrokerAllocationLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed broker allocation envelope" : error);
        return {BrokerAllocationLoadStatus::invalid_record, std::nullopt};
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {BrokerAllocationLoadStatus::io_error, std::nullopt};
    }
    if (status == ReadStatus::valid) {
        set_error(last_error_, "broker allocation filename collision");
        return {BrokerAllocationLoadStatus::invalid_record, std::nullopt};
    }
    return {BrokerAllocationLoadStatus::not_found, std::nullopt};
#else
    (void)key;
    set_error(last_error_, "WindowsFileBrokerAllocationStore requires Windows");
    return {BrokerAllocationLoadStatus::io_error, std::nullopt};
#endif
}

BrokerAllocationScanResult WindowsFileBrokerAllocationStore::scan() const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "broker allocation store is unavailable");
        return {BrokerAllocationScanStatus::io_error, {}};
    }
    FileLock lock(directory_ / L".broker-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {BrokerAllocationScanStatus::io_error, {}};
    }
    std::vector<BrokerAllocationEnvelope> records;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" ||
                filename.rfind(L"env-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                set_error(last_error_, "broker allocation envelope is not a regular file");
                return {BrokerAllocationScanStatus::invalid_record, {}};
            }
            std::optional<BrokerAllocationEnvelope> record;
            std::string error;
            const auto status = read_record_file(entry.path(), record, error);
            if (status == ReadStatus::malformed) {
                set_error(last_error_, error.empty() ? "malformed broker allocation envelope"
                                                      : error);
                return {BrokerAllocationScanStatus::invalid_record, {}};
            }
            if (status != ReadStatus::valid || !record) {
                set_error(last_error_, error.empty() ? "could not read broker allocation envelope"
                                                      : error);
                return {BrokerAllocationScanStatus::io_error, {}};
            }
            if (record_path(directory_, record->key()).filename() != entry.path().filename() ||
                !record->valid()) {
                set_error(last_error_, "broker allocation identity or metadata mismatch");
                return {BrokerAllocationScanStatus::invalid_record, {}};
            }
            records.push_back(std::move(*record));
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {BrokerAllocationScanStatus::io_error, {}};
    }
    std::sort(records.begin(), records.end(),
              [](const BrokerAllocationEnvelope &left,
                 const BrokerAllocationEnvelope &right) {
                  return left.key() < right.key();
              });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].key() == records[index].key()) {
            set_error(last_error_, "duplicate broker allocation key");
            return {BrokerAllocationScanStatus::invalid_record, {}};
        }
    }
    return {BrokerAllocationScanStatus::complete, std::move(records)};
#else
    set_error(last_error_, "WindowsFileBrokerAllocationStore requires Windows");
    return {BrokerAllocationScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

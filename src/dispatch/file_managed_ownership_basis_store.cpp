/// \file file_managed_ownership_basis_store.cpp
/// \brief Implements the Windows-backed managed-ownership-basis store.

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <mt5bridge/dispatch/file_managed_ownership_basis_store.hpp>

#include "file_store_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'O', 'W', 'N', '0', '1'}};
// Version 2 removes the non-proof-bearing managed-volume cap from the body.
// Older records are rejected rather than reinterpreted as associations.
constexpr std::uint32_t kFormatVersion = 2;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes = 768U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

using namespace file_store_support;

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
    const ManagedOwnershipBasis &record) {
    if (!record.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(256 + record.account.server.size());
    if (!append_string(body, record.account.server) ||
        !append_string(body, record.basis_key.broker_key.account.server) ||
        !append_string(body, record.basis_key.operation_key.account.server))
        return std::nullopt;
    append_u64(body, record.account.login);
    append_u64(body, record.basis_key.broker_key.account.login);
    append_u64(body, record.basis_key.broker_key.deal_ticket);
    append_u64(body, record.basis_key.operation_key.account.login);
    append_u64(body, record.basis_key.operation_key.trade_id);
    append_u64(body, record.basis_key.operation_key.operation_id);
    append_provenance(body, record.broker_provenance);
    append_u64(body, record.operation_revision);
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> serialize_record(
    const ManagedOwnershipBasis &record) {
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

std::optional<ManagedOwnershipBasis> deserialize_record(
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

    ManagedOwnershipBasis record;
    std::size_t body_offset = 0;
    if (!read_string(body, body_offset, record.account.server) ||
        !read_string(body, body_offset, record.basis_key.broker_key.account.server) ||
        !read_string(body, body_offset, record.basis_key.operation_key.account.server) ||
        !read_u64(body, body_offset, record.account.login) ||
        !read_u64(body, body_offset, record.basis_key.broker_key.account.login) ||
        !read_u64(body, body_offset, record.basis_key.broker_key.deal_ticket) ||
        !read_u64(body, body_offset, record.basis_key.operation_key.account.login) ||
        !read_u64(body, body_offset, record.basis_key.operation_key.trade_id) ||
        !read_u64(body, body_offset, record.basis_key.operation_key.operation_id) ||
        !read_provenance(body, body_offset, record.broker_provenance) ||
        !read_u64(body, body_offset, record.operation_revision) ||
        body_offset != body.size())
        return std::nullopt;
    return record.valid() ? std::optional<ManagedOwnershipBasis>(std::move(record))
                          : std::nullopt;
}

#if defined(_WIN32)

ReadStatus read_record_file(const std::filesystem::path &path,
                            std::optional<ManagedOwnershipBasis> &record,
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
                                  const ManagedOwnershipBasisKey &key) {
    const std::string suffix = "own-" + hex_u64(fnv1a(key.broker_key.account.server)) + "-" +
                               hex_u64(key.broker_key.account.login) + "-" +
                               hex_u64(key.broker_key.deal_ticket) + "-" +
                               hex_u64(key.operation_key.trade_id) + "-" +
                               hex_u64(key.operation_key.operation_id) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

} // namespace

WindowsFileManagedOwnershipBasisStore::WindowsFileManagedOwnershipBasisStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "managed ownership basis directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "managed ownership basis path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    (void)directory_;
    set_error(last_error_, "WindowsFileManagedOwnershipBasisStore requires Windows");
#endif
}

ManagedOwnershipBasisCommitStatus WindowsFileManagedOwnershipBasisStore::commit(
    const ManagedOwnershipBasis &record,
    const DurableJournalStore &journal_store,
    const DurableBrokerAllocationStore &allocation_store) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed ownership basis store is unavailable");
        return ManagedOwnershipBasisCommitStatus::io_error;
    }
    const auto proof = validate_managed_ownership_basis(record, journal_store,
                                                        allocation_store);
    if (proof != ManagedOwnershipBasisProofStatus::valid) {
        switch (proof) {
        case ManagedOwnershipBasisProofStatus::invalid_record:
            set_error(last_error_, "invalid managed ownership basis");
            return ManagedOwnershipBasisCommitStatus::invalid_record;
        case ManagedOwnershipBasisProofStatus::missing_operation_record:
            set_error(last_error_, "durable operation record is missing");
            return ManagedOwnershipBasisCommitStatus::missing_operation_record;
        case ManagedOwnershipBasisProofStatus::invalid_operation_record:
            set_error(last_error_, "durable operation is not an ownership basis");
            return ManagedOwnershipBasisCommitStatus::invalid_operation_record;
        case ManagedOwnershipBasisProofStatus::missing_allocation_envelope:
            set_error(last_error_, "durable broker allocation envelope is missing");
            return ManagedOwnershipBasisCommitStatus::missing_allocation_envelope;
        case ManagedOwnershipBasisProofStatus::invalid_allocation_envelope:
            set_error(last_error_, "durable broker allocation envelope does not match");
            return ManagedOwnershipBasisCommitStatus::invalid_allocation_envelope;
        case ManagedOwnershipBasisProofStatus::io_error:
            set_error(last_error_, "could not load ownership basis source proof");
            return ManagedOwnershipBasisCommitStatus::io_error;
        case ManagedOwnershipBasisProofStatus::valid:
            break;
        }
    }
    const auto bytes = serialize_record(record);
    if (!bytes) {
        set_error(last_error_, "managed ownership basis exceeds storage limits");
        return ManagedOwnershipBasisCommitStatus::invalid_record;
    }
    FileLock lock(directory_ / L".managed-ownership-basis.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return ManagedOwnershipBasisCommitStatus::io_error;
    }
    const auto target = record_path(directory_, record.key());
    std::optional<ManagedOwnershipBasis> existing;
    std::string read_error;
    const auto status = read_record_file(target, existing, read_error);
    if (status == ReadStatus::malformed) {
        set_error(last_error_, "malformed managed ownership basis");
        return ManagedOwnershipBasisCommitStatus::invalid_record;
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return ManagedOwnershipBasisCommitStatus::io_error;
    }
    if (status == ReadStatus::valid && existing) {
        if (*existing == record)
            return ManagedOwnershipBasisCommitStatus::already_committed;
        return ManagedOwnershipBasisCommitStatus::conflict;
    }
    const std::filesystem::path temporary(target.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, last_error_))
        return ManagedOwnershipBasisCommitStatus::io_error;
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = win32_error("MoveFileExW(managed ownership basis)");
        DeleteFileW(temporary.c_str());
        return ManagedOwnershipBasisCommitStatus::io_error;
    }
    return ManagedOwnershipBasisCommitStatus::committed;
#else
    (void)record;
    (void)journal_store;
    (void)allocation_store;
    set_error(last_error_, "WindowsFileManagedOwnershipBasisStore requires Windows");
    return ManagedOwnershipBasisCommitStatus::io_error;
#endif
}

ManagedOwnershipBasisLoadResult WindowsFileManagedOwnershipBasisStore::load(
    const ManagedOwnershipBasisKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed ownership basis store is unavailable");
        return {ManagedOwnershipBasisLoadStatus::io_error, std::nullopt};
    }
    if (!key.valid())
        return {ManagedOwnershipBasisLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L".managed-ownership-basis.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedOwnershipBasisLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedOwnershipBasis> record;
    std::string error;
    const auto status = read_record_file(record_path(directory_, key), record, error);
    if (status == ReadStatus::valid && record && record->key() == key)
        return {ManagedOwnershipBasisLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed managed ownership basis" : error);
        return {ManagedOwnershipBasisLoadStatus::invalid_record, std::nullopt};
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {ManagedOwnershipBasisLoadStatus::io_error, std::nullopt};
    }
    if (status == ReadStatus::valid) {
        set_error(last_error_, "managed ownership basis filename collision");
        return {ManagedOwnershipBasisLoadStatus::invalid_record, std::nullopt};
    }
    return {ManagedOwnershipBasisLoadStatus::not_found, std::nullopt};
#else
    (void)key;
    set_error(last_error_, "WindowsFileManagedOwnershipBasisStore requires Windows");
    return {ManagedOwnershipBasisLoadStatus::io_error, std::nullopt};
#endif
}

ManagedOwnershipBasisScanResult WindowsFileManagedOwnershipBasisStore::scan() const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed ownership basis store is unavailable");
        return {ManagedOwnershipBasisScanStatus::io_error, {}};
    }
    FileLock lock(directory_ / L".managed-ownership-basis.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedOwnershipBasisScanStatus::io_error, {}};
    }
    std::vector<ManagedOwnershipBasis> records;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" || filename.rfind(L"own-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                set_error(last_error_, "managed ownership basis is not a regular file");
                return {ManagedOwnershipBasisScanStatus::invalid_record, {}};
            }
            std::optional<ManagedOwnershipBasis> record;
            std::string error;
            const auto status = read_record_file(entry.path(), record, error);
            if (status == ReadStatus::malformed) {
                set_error(last_error_, error.empty() ? "malformed managed ownership basis"
                                                      : error);
                return {ManagedOwnershipBasisScanStatus::invalid_record, {}};
            }
            if (status != ReadStatus::valid || !record) {
                set_error(last_error_, error.empty() ? "could not read managed ownership basis"
                                                      : error);
                return {ManagedOwnershipBasisScanStatus::io_error, {}};
            }
            if (record_path(directory_, record->key()).filename() != entry.path().filename() ||
                !record->valid()) {
                set_error(last_error_, "managed ownership basis identity or metadata mismatch");
                return {ManagedOwnershipBasisScanStatus::invalid_record, {}};
            }
            records.push_back(std::move(*record));
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {ManagedOwnershipBasisScanStatus::io_error, {}};
    }
    std::sort(records.begin(), records.end(),
              [](const ManagedOwnershipBasis &left, const ManagedOwnershipBasis &right) {
                  return left.key() < right.key();
              });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].key() == records[index].key()) {
            set_error(last_error_, "duplicate managed ownership basis key");
            return {ManagedOwnershipBasisScanStatus::invalid_record, {}};
        }
    }
    return {ManagedOwnershipBasisScanStatus::complete, std::move(records)};
#else
    set_error(last_error_, "WindowsFileManagedOwnershipBasisStore requires Windows");
    return {ManagedOwnershipBasisScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

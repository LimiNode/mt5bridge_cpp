/// \file file_managed_deal_settlement_store.cpp
/// \brief Implements the Windows-backed managed deal settlement store.

#include <algorithm>
#include <array>
#include <utility>

#include <mt5bridge/dispatch/file_managed_deal_settlement_store.hpp>

#include "file_store_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'M', 'D', 'S', '0', '1'}};
constexpr std::array<char, 8> kPendingMagic{{'M', 'T', '5', 'M', 'D', 'P', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxEntries = 4096;
constexpr std::size_t kMaxBodyBytes = 1U << 20;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

struct PendingPublication {
    ManagedDealSettlement fact;
    ManagedSettlementFrontier frontier;
};

using namespace file_store_support;

void append_provenance(std::vector<std::uint8_t> &bytes,
                       const ManagedSettlementProvenance &provenance) {
    append_u64(bytes, provenance.graph_instance_id);
    append_u64(bytes, provenance.graph_revision);
    append_u64(bytes, provenance.history_deals_revision);
    append_u64(bytes, provenance.evidence_digest);
}

bool read_provenance(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                     ManagedSettlementProvenance &provenance) {
    return read_u64(bytes, offset, provenance.graph_instance_id) &&
           read_u64(bytes, offset, provenance.graph_revision) &&
           read_u64(bytes, offset, provenance.history_deals_revision) &&
           read_u64(bytes, offset, provenance.evidence_digest);
}

std::optional<std::vector<std::uint8_t>> serialize_body(
    const ManagedDealSettlement &record) {
    if (!record.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_string(body, record.key.account.server) ||
        !append_string(body, record.key.operation_key.account.server))
        return std::nullopt;
    append_u64(body, record.key.account.login);
    append_u64(body, record.key.operation_key.account.login);
    append_u64(body, record.key.operation_key.trade_id);
    append_u64(body, record.key.operation_key.operation_id);
    append_u64(body, record.key.deal_ticket);
    append_u64(body, record.source_operation_revision);
    append_u64(body, record.managed_logical_units);
    append_provenance(body, record.provenance);
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> serialize_body(
    const ManagedSettlementFrontier &record) {
    if (!record.valid() || record.entries.size() > kMaxEntries)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_string(body, record.account.server) ||
        !append_string(body, record.operation_key.account.server))
        return std::nullopt;
    append_u64(body, record.account.login);
    append_u64(body, record.operation_key.account.login);
    append_u64(body, record.operation_key.trade_id);
    append_u64(body, record.operation_key.operation_id);
    append_u64(body, record.operation_revision);
    append_u64(body, record.settled_volume);
    append_provenance(body, record.provenance);
    append_u32(body, static_cast<std::uint32_t>(record.entries.size()));
    for (const auto &entry : record.entries) {
        append_u64(body, entry.deal_ticket);
        append_u64(body, entry.source_operation_revision);
        append_u64(body, entry.managed_logical_units);
    }
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

template <typename Record>
std::optional<std::vector<std::uint8_t>> serialize_record(const Record &record) {
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

std::optional<std::vector<std::uint8_t>> serialize_pending(
    const PendingPublication &pending) {
    if (!pending.fact.valid() || !pending.frontier.valid() ||
        pending.fact.key.account != pending.frontier.account ||
        pending.fact.key.operation_key != pending.frontier.operation_key ||
        pending.fact.source_operation_revision != pending.frontier.operation_revision)
        return std::nullopt;
    const auto fact = serialize_record(pending.fact);
    const auto frontier = serialize_record(pending.frontier);
    if (!fact || !frontier || fact->size() > kMaxRecordBytes ||
        frontier->size() > kMaxRecordBytes)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    append_u64(body, static_cast<std::uint64_t>(fact->size()));
    body.insert(body.end(), fact->begin(), fact->end());
    append_u64(body, static_cast<std::uint64_t>(frontier->size()));
    body.insert(body.end(), frontier->begin(), frontier->end());
    if (body.size() > kMaxBodyBytes)
        return std::nullopt;
    std::vector<std::uint8_t> result;
    result.reserve(kEnvelopeBytes + body.size());
    result.insert(result.end(), kPendingMagic.begin(), kPendingMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body.size()));
    append_u64(result, checksum(body));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

bool read_envelope(const std::vector<std::uint8_t> &bytes,
                   const std::array<char, 8> &magic,
                   std::vector<std::uint8_t> &body) {
    if (bytes.size() < kEnvelopeBytes || bytes.size() > kMaxRecordBytes ||
        !std::equal(magic.begin(), magic.end(), bytes.begin()))
        return false;
    std::size_t offset = magic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, offset, version) || !read_u64(bytes, offset, body_size) ||
        !read_u64(bytes, offset, expected_checksum) || version != kFormatVersion ||
        body_size > kMaxBodyBytes || body_size != bytes.size() - offset)
        return false;
    body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    return checksum(body) == expected_checksum;
}

bool read_record_envelope(const std::vector<std::uint8_t> &bytes,
                          std::vector<std::uint8_t> &body) {
    return read_envelope(bytes, kMagic, body);
}

std::optional<ManagedDealSettlement> deserialize_fact(
    const std::vector<std::uint8_t> &bytes) {
    std::vector<std::uint8_t> body;
    if (!read_record_envelope(bytes, body))
        return std::nullopt;
    ManagedDealSettlement record;
    std::size_t offset = 0;
    if (!read_string(body, offset, record.key.account.server) ||
        !read_string(body, offset, record.key.operation_key.account.server) ||
        !read_u64(body, offset, record.key.account.login) ||
        !read_u64(body, offset, record.key.operation_key.account.login) ||
        !read_u64(body, offset, record.key.operation_key.trade_id) ||
        !read_u64(body, offset, record.key.operation_key.operation_id) ||
        !read_u64(body, offset, record.key.deal_ticket) ||
        !read_u64(body, offset, record.source_operation_revision) ||
        !read_u64(body, offset, record.managed_logical_units) ||
        !read_provenance(body, offset, record.provenance) || offset != body.size())
        return std::nullopt;
    return record.valid() ? std::optional<ManagedDealSettlement>(std::move(record))
                          : std::nullopt;
}

std::optional<ManagedSettlementFrontier> deserialize_frontier(
    const std::vector<std::uint8_t> &bytes) {
    std::vector<std::uint8_t> body;
    if (!read_record_envelope(bytes, body))
        return std::nullopt;
    ManagedSettlementFrontier record;
    std::size_t offset = 0;
    std::uint32_t count = 0;
    if (!read_string(body, offset, record.account.server) ||
        !read_string(body, offset, record.operation_key.account.server) ||
        !read_u64(body, offset, record.account.login) ||
        !read_u64(body, offset, record.operation_key.account.login) ||
        !read_u64(body, offset, record.operation_key.trade_id) ||
        !read_u64(body, offset, record.operation_key.operation_id) ||
        !read_u64(body, offset, record.operation_revision) ||
        !read_u64(body, offset, record.settled_volume) ||
        !read_provenance(body, offset, record.provenance) ||
        !read_u32(body, offset, count) || count == 0 || count > kMaxEntries)
        return std::nullopt;
    record.entries.reserve(count);
    for (std::uint32_t index = 0; index != count; ++index) {
        ManagedSettlementFrontierEntry entry;
        if (!read_u64(body, offset, entry.deal_ticket) ||
            !read_u64(body, offset, entry.source_operation_revision) ||
            !read_u64(body, offset, entry.managed_logical_units))
            return std::nullopt;
        record.entries.push_back(entry);
    }
    if (offset != body.size() || !record.valid())
        return std::nullopt;
    return record;
}

std::optional<PendingPublication> deserialize_pending(
    const std::vector<std::uint8_t> &bytes) {
    std::vector<std::uint8_t> body;
    if (!read_envelope(bytes, kPendingMagic, body))
        return std::nullopt;
    std::size_t offset = 0;
    std::uint64_t fact_size = 0;
    std::uint64_t frontier_size = 0;
    if (!read_u64(body, offset, fact_size) || fact_size > kMaxRecordBytes ||
        offset > body.size() || body.size() - offset < fact_size)
        return std::nullopt;
    const auto fact_begin = body.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto fact_end = fact_begin + static_cast<std::ptrdiff_t>(fact_size);
    std::vector<std::uint8_t> fact_bytes(fact_begin, fact_end);
    offset += static_cast<std::size_t>(fact_size);
    if (!read_u64(body, offset, frontier_size) || frontier_size > kMaxRecordBytes ||
        offset > body.size() || body.size() - offset != frontier_size)
        return std::nullopt;
    const auto frontier_begin = body.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto frontier_end = frontier_begin + static_cast<std::ptrdiff_t>(frontier_size);
    std::vector<std::uint8_t> frontier_bytes(frontier_begin, frontier_end);
    const auto fact = deserialize_fact(fact_bytes);
    const auto frontier = deserialize_frontier(frontier_bytes);
    if (!fact || !frontier || fact->key.account != frontier->account ||
        fact->key.operation_key != frontier->operation_key ||
        fact->source_operation_revision != frontier->operation_revision)
        return std::nullopt;
    bool referenced = false;
    for (const auto &entry : frontier->entries) {
        if (entry.deal_ticket == fact->key.deal_ticket) {
            if (entry.source_operation_revision != fact->source_operation_revision ||
                entry.managed_logical_units != fact->managed_logical_units)
                return std::nullopt;
            referenced = true;
        }
    }
    return referenced ? std::optional<PendingPublication>(
                            PendingPublication{std::move(*fact), std::move(*frontier)})
                      : std::nullopt;
}

#if defined(_WIN32)

template <typename Record, typename Decoder>
ReadStatus read_decoded_file(const std::filesystem::path &path,
                             std::optional<Record> &record, std::string &error,
                             Decoder decoder) {
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, kMaxRecordBytes, bytes, error,
                                        ReadStatus::malformed);
    if (status != ReadStatus::valid)
        return status;
    record = decoder(bytes);
    return record ? ReadStatus::valid : ReadStatus::malformed;
}

#endif

std::filesystem::path fact_path(const std::filesystem::path &directory,
                                const ManagedDealSettlementKey &key) {
    const std::string suffix =
        "deal-" + hex_u64(fnv1a(key.account.server)) + "-" + hex_u64(key.account.login) +
        "-" + hex_u64(key.operation_key.trade_id) + "-" +
        hex_u64(key.operation_key.operation_id) + "-" + hex_u64(key.deal_ticket) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

std::filesystem::path frontier_path(const std::filesystem::path &directory,
                                    const ManagedSettlementFrontier &frontier) {
    const std::string suffix =
        "frontier-" + hex_u64(fnv1a(frontier.account.server)) + "-" +
        hex_u64(frontier.account.login) + "-" + hex_u64(frontier.operation_key.trade_id) +
        "-" + hex_u64(frontier.operation_key.operation_id) + "-" +
        hex_u64(frontier.operation_revision) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

std::filesystem::path pending_path(const std::filesystem::path &directory,
                                   const PendingPublication &pending) {
    const std::string suffix =
        "pending-" + hex_u64(fnv1a(pending.fact.key.account.server)) + "-" +
        hex_u64(pending.fact.key.account.login) + "-" +
        hex_u64(pending.fact.key.operation_key.trade_id) + "-" +
        hex_u64(pending.fact.key.operation_key.operation_id) + "-" +
        hex_u64(pending.frontier.operation_revision) + "-" +
        hex_u64(pending.fact.key.deal_ticket) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

#if defined(_WIN32)

struct PendingFile {
    PendingPublication publication;
};

enum class PendingFilesStatus { complete, invalid_record, io_error };

PendingFilesStatus collect_pending_files(const std::filesystem::path &directory,
                                         std::vector<PendingFile> &pending,
                                         std::string &error) {
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" ||
                filename.rfind(L"pending-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                error = "managed deal settlement pending record is not a regular file";
                return PendingFilesStatus::invalid_record;
            }
            std::optional<PendingPublication> publication;
            const auto status = read_decoded_file(entry.path(), publication, error,
                                                  deserialize_pending);
            if (status == ReadStatus::io_error)
                return PendingFilesStatus::io_error;
            if (status != ReadStatus::valid || !publication ||
                pending_path(directory, *publication).filename() != entry.path().filename()) {
                if (error.empty())
                    error = "malformed managed deal settlement pending record";
                return PendingFilesStatus::invalid_record;
            }
            pending.push_back({std::move(*publication)});
        }
    } catch (const std::filesystem::filesystem_error &filesystem_error) {
        error = filesystem_error.what();
        return PendingFilesStatus::io_error;
    }
    return PendingFilesStatus::complete;
}

#endif

} // namespace

WindowsFileManagedDealSettlementStore::WindowsFileManagedDealSettlementStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "managed deal settlement directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "managed deal settlement path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    (void)directory_;
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
#endif
}

ManagedDealSettlementCommitStatus WindowsFileManagedDealSettlementStore::commit(
    const ManagedDealSettlementCommit &proof,
    const DurableJournalStore &journal_store) {
#if defined(_WIN32)
    const auto &fact = proof.fact();
    const auto &frontier = proof.frontier();
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed deal settlement store is unavailable");
        return ManagedDealSettlementCommitStatus::io_error;
    }
    if (!fact.valid() || !frontier.valid() || fact.key.operation_key != frontier.operation_key ||
        fact.key.account != frontier.account || fact.source_operation_revision !=
                                                     frontier.operation_revision)
        return ManagedDealSettlementCommitStatus::invalid_record;

    FileLock lock(directory_ / L".managed-deal-settlement.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return ManagedDealSettlementCommitStatus::io_error;
    }

    std::optional<ManagedDealSettlement> existing_fact;
    std::string read_error;
    const auto fact_status = read_decoded_file(
        fact_path(directory_, fact.key), existing_fact, read_error, deserialize_fact);
    if (fact_status == ReadStatus::malformed) {
        set_error(last_error_, "malformed managed deal settlement fact");
        return ManagedDealSettlementCommitStatus::invalid_record;
    }
    if (fact_status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return ManagedDealSettlementCommitStatus::io_error;
    }
    if (fact_status == ReadStatus::valid && existing_fact && !(*existing_fact == fact))
        return ManagedDealSettlementCommitStatus::conflict;

    std::optional<ManagedSettlementFrontier> existing_frontier;
    read_error.clear();
    const auto frontier_status = read_decoded_file(
        frontier_path(directory_, frontier), existing_frontier, read_error,
        deserialize_frontier);
    if (frontier_status == ReadStatus::malformed) {
        set_error(last_error_, "malformed managed settlement frontier");
        return ManagedDealSettlementCommitStatus::invalid_record;
    }
    if (frontier_status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return ManagedDealSettlementCommitStatus::io_error;
    }
    if (frontier_status == ReadStatus::valid && existing_frontier &&
        !(*existing_frontier == frontier))
        return ManagedDealSettlementCommitStatus::conflict;

    PendingPublication pending{fact, frontier};
    std::optional<PendingPublication> existing_pending;
    read_error.clear();
    const auto pending_status = read_decoded_file(
        pending_path(directory_, pending), existing_pending, read_error,
        deserialize_pending);
    if (pending_status == ReadStatus::malformed) {
        set_error(last_error_, "malformed managed deal settlement pending record");
        return ManagedDealSettlementCommitStatus::invalid_record;
    }
    if (pending_status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return ManagedDealSettlementCommitStatus::io_error;
    }
    if (pending_status == ReadStatus::valid && existing_pending &&
        (!(existing_pending->fact == pending.fact) ||
         !(existing_pending->frontier == pending.frontier)))
        return ManagedDealSettlementCommitStatus::conflict;

    bool candidate_referenced = false;
    for (const auto &entry : frontier.entries) {
        if (entry.deal_ticket == fact.key.deal_ticket) {
            candidate_referenced = true;
            if (entry.source_operation_revision != fact.source_operation_revision ||
                entry.managed_logical_units != fact.managed_logical_units)
                return ManagedDealSettlementCommitStatus::invalid_record;
            continue;
        }
        ManagedDealSettlementKey referenced{frontier.account, frontier.operation_key,
                                            entry.deal_ticket};
        std::optional<ManagedDealSettlement> referenced_fact;
        read_error.clear();
        const auto status = read_decoded_file(
            fact_path(directory_, referenced), referenced_fact, read_error, deserialize_fact);
        if (status == ReadStatus::absent)
            return ManagedDealSettlementCommitStatus::missing_fact;
        if (status == ReadStatus::malformed) {
            set_error(last_error_, "malformed referenced managed deal settlement fact");
            return ManagedDealSettlementCommitStatus::invalid_record;
        }
        if (status == ReadStatus::io_error) {
            set_error(last_error_, read_error);
            return ManagedDealSettlementCommitStatus::io_error;
        }
        if (!referenced_fact || !(referenced_fact->key == referenced) ||
            referenced_fact->source_operation_revision != entry.source_operation_revision ||
            referenced_fact->managed_logical_units != entry.managed_logical_units)
            return ManagedDealSettlementCommitStatus::invalid_record;
    }
    if (!candidate_referenced)
        return ManagedDealSettlementCommitStatus::invalid_record;

    if (frontier_status == ReadStatus::valid && fact_status == ReadStatus::valid)
        return ManagedDealSettlementCommitStatus::already_committed;

    const auto operation = journal_store.load(frontier.operation_key);
    if (operation.status == StoreLoadStatus::not_found)
        return ManagedDealSettlementCommitStatus::missing_operation_record;
    if (operation.status == StoreLoadStatus::invalid_record ||
        operation.status == StoreLoadStatus::io_error || !operation.record)
        return operation.status == StoreLoadStatus::io_error
                   ? ManagedDealSettlementCommitStatus::io_error
                   : ManagedDealSettlementCommitStatus::invalid_operation_record;
    const auto &source = *operation.record;
    if (!source.valid() || source.key != frontier.operation_key ||
        source.revision != frontier.operation_revision ||
        source.operation_kind != OperationKind::close ||
        (source.operation_state != OperationState::partially_filled &&
         source.operation_state != OperationState::filled) ||
        !journal_at_least_result_persisted(source.journal_state) ||
        !source.reconciliation_descriptor || source.reconciliation_descriptor->requested_volume == 0 ||
        source.settled_volume == 0 || source.settled_volume != frontier.settled_volume ||
        source.settled_volume > source.reconciliation_descriptor->requested_volume)
        return ManagedDealSettlementCommitStatus::invalid_operation_record;

    if (pending_status == ReadStatus::absent) {
        const auto bytes = serialize_pending(pending);
        if (!bytes) {
            set_error(last_error_, "managed deal settlement pending record exceeds storage limits");
            return ManagedDealSettlementCommitStatus::invalid_record;
        }
        const auto target = pending_path(directory_, pending);
        const std::filesystem::path temporary(target.wstring() + L".tmp");
        DeleteFileW(temporary.c_str());
        if (!write_file(temporary, *bytes, last_error_))
            return ManagedDealSettlementCommitStatus::io_error;
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            last_error_ = win32_error("MoveFileExW(managed deal settlement pending)");
            DeleteFileW(temporary.c_str());
            return ManagedDealSettlementCommitStatus::io_error;
        }
    }

    if (fact_status == ReadStatus::absent) {
        const auto bytes = serialize_record(fact);
        if (!bytes) {
            set_error(last_error_, "managed deal settlement fact exceeds storage limits");
            return ManagedDealSettlementCommitStatus::invalid_record;
        }
        const auto target = fact_path(directory_, fact.key);
        const std::filesystem::path temporary(target.wstring() + L".tmp");
        DeleteFileW(temporary.c_str());
        if (!write_file(temporary, *bytes, last_error_))
            return ManagedDealSettlementCommitStatus::io_error;
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            last_error_ = win32_error("MoveFileExW(managed deal settlement fact)");
            DeleteFileW(temporary.c_str());
            return ManagedDealSettlementCommitStatus::io_error;
        }
    }

    // The frontier is the publication marker. A crash before it is published
    // leaves a recoverable pending fact, never a partially committed frontier.
    if (frontier_status == ReadStatus::absent) {
        const auto bytes = serialize_record(frontier);
        if (!bytes) {
            set_error(last_error_, "managed settlement frontier exceeds storage limits");
            return ManagedDealSettlementCommitStatus::invalid_record;
        }
        const auto target = frontier_path(directory_, frontier);
        const std::filesystem::path temporary(target.wstring() + L".tmp");
        DeleteFileW(temporary.c_str());
        if (!write_file(temporary, *bytes, last_error_))
            return ManagedDealSettlementCommitStatus::io_error;
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            last_error_ = win32_error("MoveFileExW(managed settlement frontier)");
            DeleteFileW(temporary.c_str());
            return ManagedDealSettlementCommitStatus::io_error;
        }
    }
    return ManagedDealSettlementCommitStatus::committed;
#else
    (void)proof;
    (void)journal_store;
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
    return ManagedDealSettlementCommitStatus::io_error;
#endif
}

ManagedDealSettlementCommitStatus WindowsFileManagedDealSettlementStore::recover_pending(
    const DurableJournalStore &journal_store) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed deal settlement store is unavailable");
        return ManagedDealSettlementCommitStatus::io_error;
    }

    std::vector<PendingFile> pending_files;
    {
        FileLock lock(directory_ / L".managed-deal-settlement.lock");
        if (!lock.acquired()) {
            last_error_ = lock.error();
            return ManagedDealSettlementCommitStatus::io_error;
        }
        std::string error;
        const auto status = collect_pending_files(directory_, pending_files, error);
        if (status != PendingFilesStatus::complete) {
            set_error(last_error_, error.empty()
                                     ? "unable to read managed deal settlement pending records"
                                     : error);
            return status == PendingFilesStatus::io_error
                       ? ManagedDealSettlementCommitStatus::io_error
                       : ManagedDealSettlementCommitStatus::invalid_record;
        }
    }
    if (pending_files.empty())
        return ManagedDealSettlementCommitStatus::no_pending;

    std::vector<bool> recovered(pending_files.size(), false);
    std::size_t remaining = pending_files.size();
    bool committed = false;
    while (remaining != 0) {
        bool progress = false;
        ManagedDealSettlementCommitStatus deferred =
            ManagedDealSettlementCommitStatus::missing_fact;
        for (std::size_t index = 0; index != pending_files.size(); ++index) {
            if (recovered[index])
                continue;
            auto &publication = pending_files[index].publication;
            ManagedDealSettlementCommit proof(publication.fact, publication.frontier);
            const auto status = commit(proof, journal_store);
            if (status == ManagedDealSettlementCommitStatus::missing_fact) {
                deferred = status;
                continue;
            }
            if (status != ManagedDealSettlementCommitStatus::committed &&
                status != ManagedDealSettlementCommitStatus::already_committed)
                return status;
            recovered[index] = true;
            --remaining;
            progress = true;
            committed = committed || status == ManagedDealSettlementCommitStatus::committed;
        }
        if (!progress)
            return deferred;
    }
    return committed ? ManagedDealSettlementCommitStatus::committed
                     : ManagedDealSettlementCommitStatus::already_committed;
#else
    (void)journal_store;
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
    return ManagedDealSettlementCommitStatus::io_error;
#endif
}

ManagedDealSettlementLoadResult WindowsFileManagedDealSettlementStore::load(
    const ManagedDealSettlementKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed deal settlement store is unavailable");
        return {ManagedDealSettlementLoadStatus::io_error, std::nullopt};
    }
    if (!key.valid())
        return {ManagedDealSettlementLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L".managed-deal-settlement.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedDealSettlementLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedDealSettlement> record;
    std::string error;
    const auto status = read_decoded_file(fact_path(directory_, key), record, error,
                                          deserialize_fact);
    if (status == ReadStatus::valid && record && record->key == key)
        return {ManagedDealSettlementLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed managed deal settlement fact" : error);
        return {ManagedDealSettlementLoadStatus::invalid_record, std::nullopt};
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {ManagedDealSettlementLoadStatus::io_error, std::nullopt};
    }
    if (status == ReadStatus::valid) {
        set_error(last_error_, "managed deal settlement filename collision");
        return {ManagedDealSettlementLoadStatus::invalid_record, std::nullopt};
    }
    return {ManagedDealSettlementLoadStatus::not_found, std::nullopt};
#else
    (void)key;
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
    return {ManagedDealSettlementLoadStatus::io_error, std::nullopt};
#endif
}

ManagedDealSettlementScanResult WindowsFileManagedDealSettlementStore::scan() const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed deal settlement store is unavailable");
        return {ManagedDealSettlementScanStatus::io_error, {}, {}};
    }
    FileLock lock(directory_ / L".managed-deal-settlement.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedDealSettlementScanStatus::io_error, {}, {}};
    }
    ManagedDealSettlementScanResult result;
    std::vector<PendingFile> pending_files;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin")
                continue;
            if (!entry.is_regular_file()) {
                set_error(last_error_, "managed settlement record is not a regular file");
                return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
            }
            std::string error;
            if (filename.rfind(L"deal-", 0) == 0) {
                std::optional<ManagedDealSettlement> fact;
                const auto status = read_decoded_file(entry.path(), fact, error,
                                                       deserialize_fact);
                if (status != ReadStatus::valid || !fact) {
                    set_error(last_error_, error.empty() ? "malformed managed deal settlement"
                                                          : error);
                    return {status == ReadStatus::io_error
                                ? ManagedDealSettlementScanStatus::io_error
                                : ManagedDealSettlementScanStatus::invalid_record,
                            {}, {}};
                }
                if (fact_path(directory_, fact->key).filename() != entry.path().filename()) {
                    set_error(last_error_, "managed deal settlement filename mismatch");
                    return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
                }
                result.facts.push_back(std::move(*fact));
            } else if (filename.rfind(L"frontier-", 0) == 0) {
                std::optional<ManagedSettlementFrontier> frontier;
                const auto status = read_decoded_file(entry.path(), frontier, error,
                                                       deserialize_frontier);
                if (status != ReadStatus::valid || !frontier) {
                    set_error(last_error_, error.empty() ? "malformed settlement frontier"
                                                          : error);
                    return {status == ReadStatus::io_error
                                ? ManagedDealSettlementScanStatus::io_error
                                : ManagedDealSettlementScanStatus::invalid_record,
                            {}, {}};
                }
                if (frontier_path(directory_, *frontier).filename() != entry.path().filename()) {
                    set_error(last_error_, "settlement frontier filename mismatch");
                    return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
                }
                result.frontiers.push_back(std::move(*frontier));
            }
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {ManagedDealSettlementScanStatus::io_error, {}, {}};
    }
    std::string pending_error;
    const auto pending_status =
        collect_pending_files(directory_, pending_files, pending_error);
    if (pending_status != PendingFilesStatus::complete) {
        set_error(last_error_, pending_error.empty()
                                 ? "unable to read managed deal settlement pending records"
                                 : pending_error);
        return {pending_status == PendingFilesStatus::io_error
                    ? ManagedDealSettlementScanStatus::io_error
                    : ManagedDealSettlementScanStatus::invalid_record,
                {}, {}};
    }
    std::sort(result.facts.begin(), result.facts.end(),
              [](const auto &left, const auto &right) { return left.key < right.key; });
    std::sort(result.frontiers.begin(), result.frontiers.end(),
              [](const auto &left, const auto &right) {
                  if (left.operation_key != right.operation_key)
                      return left.operation_key < right.operation_key;
                  return left.operation_revision < right.operation_revision;
              });
    for (std::size_t index = 1; index < result.facts.size(); ++index) {
        if (result.facts[index - 1].key == result.facts[index].key) {
            set_error(last_error_, "duplicate managed deal settlement fact");
            return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
        }
    }
    for (std::size_t index = 1; index < result.frontiers.size(); ++index) {
        if (result.frontiers[index - 1].operation_key == result.frontiers[index].operation_key &&
            result.frontiers[index - 1].operation_revision ==
                result.frontiers[index].operation_revision) {
            set_error(last_error_, "duplicate settlement frontier");
            return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
        }
    }
    for (const auto &frontier : result.frontiers) {
        for (const auto &entry : frontier.entries) {
            const ManagedDealSettlementKey key{frontier.account, frontier.operation_key,
                                               entry.deal_ticket};
            const auto found = std::find_if(
                result.facts.begin(), result.facts.end(),
                [&key](const auto &fact) { return fact.key == key; });
            if (found == result.facts.end() ||
                found->source_operation_revision != entry.source_operation_revision ||
                found->managed_logical_units != entry.managed_logical_units) {
                set_error(last_error_, "settlement frontier references a missing or mismatched fact");
                return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
            }
        }
    }
    for (const auto &pending : pending_files) {
        const auto fact = std::find_if(
            result.facts.begin(), result.facts.end(),
            [&pending](const auto &candidate) {
                return candidate.key == pending.publication.fact.key;
            });
        if (fact != result.facts.end() && !(*fact == pending.publication.fact)) {
            set_error(last_error_, "pending managed deal settlement fact conflicts with published fact");
            return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
        }
        const auto frontier = std::find_if(
            result.frontiers.begin(), result.frontiers.end(),
            [&pending](const auto &candidate) {
                return candidate.operation_key == pending.publication.frontier.operation_key &&
                       candidate.operation_revision ==
                           pending.publication.frontier.operation_revision;
            });
        if (frontier != result.frontiers.end() &&
            !(*frontier == pending.publication.frontier)) {
            set_error(last_error_,
                      "pending managed settlement frontier conflicts with published frontier");
            return {ManagedDealSettlementScanStatus::invalid_record, {}, {}};
        }
    }
    std::vector<ManagedDealSettlement> committed_facts;
    std::vector<ManagedDealSettlement> pending_facts;
    committed_facts.reserve(result.facts.size());
    pending_facts.reserve(result.facts.size());
    for (const auto &fact : result.facts) {
        const auto referenced = std::any_of(
            result.frontiers.begin(), result.frontiers.end(),
            [&fact](const auto &frontier) {
                if (frontier.account != fact.key.account ||
                    frontier.operation_key != fact.key.operation_key)
                    return false;
                return std::any_of(
                    frontier.entries.begin(), frontier.entries.end(),
                    [&fact](const auto &entry) {
                        return entry.deal_ticket == fact.key.deal_ticket &&
                               entry.source_operation_revision ==
                                   fact.source_operation_revision &&
                               entry.managed_logical_units == fact.managed_logical_units;
                    });
            });
        (referenced ? committed_facts : pending_facts).push_back(fact);
    }
    result.facts.swap(committed_facts);
    result.pending_facts.swap(pending_facts);
    const auto append_pending_fact = [&result](const ManagedDealSettlement &fact) {
        const auto found = std::find_if(
            result.pending_facts.begin(), result.pending_facts.end(),
            [&fact](const auto &candidate) { return candidate.key == fact.key; });
        if (found == result.pending_facts.end())
            result.pending_facts.push_back(fact);
    };
    for (const auto &pending : pending_files) {
        const auto fact = std::find_if(
            result.facts.begin(), result.facts.end(),
            [&pending](const auto &candidate) {
                return candidate.key == pending.publication.fact.key;
            });
        const auto frontier = std::find_if(
            result.frontiers.begin(), result.frontiers.end(),
            [&pending](const auto &candidate) {
                return candidate.operation_key == pending.publication.frontier.operation_key &&
                       candidate.operation_revision ==
                           pending.publication.frontier.operation_revision;
            });
        if (fact == result.facts.end() || frontier == result.frontiers.end())
            append_pending_fact(pending.publication.fact);
    }
    result.status = ManagedDealSettlementScanStatus::complete;
    return result;
#else
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
    return {ManagedDealSettlementScanStatus::io_error, {}, {}};
#endif
}

} // namespace mt5bridge

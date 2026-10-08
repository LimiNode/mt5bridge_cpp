/// \file file_managed_deal_settlement_store.cpp
/// \brief Implements the Windows-backed managed deal settlement store.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <mt5bridge/dispatch/file_managed_deal_settlement_store.hpp>

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'M', 'D', 'S', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxStringBytes = 1U << 20;
constexpr std::size_t kMaxEntries = 4096;
constexpr std::size_t kMaxBodyBytes = 1U << 20;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

std::uint64_t checksum(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::uint64_t fnv1a(const std::string &value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::wstring widen_ascii(const std::string &value) {
    return std::wstring(value.begin(), value.end());
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint32_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t))
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return true;
}

bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint64_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t))
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

bool append_string(std::vector<std::uint8_t> &bytes, const std::string &value) {
    if (value.size() > kMaxStringBytes ||
        value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool read_string(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 std::string &value) {
    std::uint32_t size = 0;
    if (!read_u32(bytes, offset, size) || size > kMaxStringBytes ||
        offset > bytes.size() || bytes.size() - offset < size)
        return false;
    value.assign(reinterpret_cast<const char *>(bytes.data() + offset), size);
    offset += size;
    return true;
}

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

bool read_envelope(const std::vector<std::uint8_t> &bytes,
                   std::vector<std::uint8_t> &body) {
    if (bytes.size() < kEnvelopeBytes || bytes.size() > kMaxRecordBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return false;
    std::size_t offset = kMagic.size();
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

std::optional<ManagedDealSettlement> deserialize_fact(
    const std::vector<std::uint8_t> &bytes) {
    std::vector<std::uint8_t> body;
    if (!read_envelope(bytes, body))
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
    if (!read_envelope(bytes, body))
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

#if defined(_WIN32)

std::string win32_error(const char *operation, DWORD error = GetLastError()) {
    return std::string(operation) + " failed (Win32 error " + std::to_string(error) + ")";
}

class FileLock final {
public:
    explicit FileLock(const std::filesystem::path &path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error_ = win32_error("CreateFileW(lock)");
            return;
        }
        OVERLAPPED overlapped{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                        &overlapped)) {
            error_ = win32_error("LockFileEx");
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    ~FileLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
    }
    bool acquired() const { return handle_ != INVALID_HANDLE_VALUE; }
    const std::string &error() const { return error_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string error_;
};

enum class ReadStatus { absent, valid, malformed, io_error };

ReadStatus read_bytes_file(const std::filesystem::path &path,
                           std::vector<std::uint8_t> &bytes, std::string &error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return ReadStatus::absent;
        error = win32_error("CreateFileW(record)", code);
        return ReadStatus::io_error;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxRecordBytes) {
        error = "managed deal settlement has an invalid size";
        CloseHandle(handle);
        return ReadStatus::malformed;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data() + offset, requested, &read, nullptr) || read == 0) {
            error = win32_error("ReadFile");
            CloseHandle(handle);
            return ReadStatus::io_error;
        }
        offset += read;
    }
    CloseHandle(handle);
    return ReadStatus::valid;
}

template <typename Record, typename Decoder>
ReadStatus read_decoded_file(const std::filesystem::path &path,
                             std::optional<Record> &record, std::string &error,
                             Decoder decoder) {
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, bytes, error);
    if (status != ReadStatus::valid)
        return status;
    record = decoder(bytes);
    return record ? ReadStatus::valid : ReadStatus::malformed;
}

bool write_file(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes, std::string &error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = win32_error("CreateFileW(temp)");
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            error = win32_error("WriteFile");
            CloseHandle(handle);
            DeleteFileW(path.c_str());
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        error = win32_error("FlushFileBuffers");
        CloseHandle(handle);
        DeleteFileW(path.c_str());
        return false;
    }
    CloseHandle(handle);
    return true;
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

void set_error(std::string &target, std::string value) { target = std::move(value); }

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
    result.status = ManagedDealSettlementScanStatus::complete;
    return result;
#else
    set_error(last_error_, "WindowsFileManagedDealSettlementStore requires Windows");
    return {ManagedDealSettlementScanStatus::io_error, {}, {}};
#endif
}

} // namespace mt5bridge

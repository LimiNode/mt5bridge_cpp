/// \file file_managed_allocation_store.cpp
/// \brief Implements the Windows-backed managed-allocation store.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <mt5bridge/dispatch/file_managed_allocation_store.hpp>

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'A', 'L', 'C', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxStringBytes = 1U << 20;
constexpr std::size_t kMaxBodyBytes = 1024U;
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

void append_volume(std::vector<std::uint8_t> &bytes,
                   const ManagedAllocationVolume &volume) {
    append_u64(bytes, volume.units);
    append_u32(bytes, volume.scale);
    append_u64(bytes, volume.step_units);
}

bool read_volume(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 ManagedAllocationVolume &volume) {
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
    const ManagedAllocationRecord &record) {
    if (!record.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(512 + record.account.server.size() + record.broker_key.account.server.size() +
                 record.operation_key.account.server.size());
    if (!append_string(body, record.account.server) ||
        !append_string(body, record.broker_key.account.server) ||
        !append_string(body, record.operation_key.account.server))
        return std::nullopt;
    append_u64(body, record.account.login);
    append_u64(body, record.broker_key.account.login);
    append_u64(body, record.broker_key.deal_ticket);
    append_u64(body, record.operation_key.account.login);
    append_u64(body, record.operation_key.trade_id);
    append_u64(body, record.operation_key.operation_id);
    append_provenance(body, record.broker_provenance);
    append_volume(body, record.managed_close_leg);
    append_volume(body, record.managed_reverse_open_leg);
    append_volume(body, record.unallocated_close_leg);
    append_volume(body, record.unallocated_reverse_open_leg);
    return body.size() <= kMaxBodyBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(body))
               : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> serialize_record(
    const ManagedAllocationRecord &record) {
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

std::optional<ManagedAllocationRecord> deserialize_record(
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

    ManagedAllocationRecord record;
    std::size_t body_offset = 0;
    if (!read_string(body, body_offset, record.account.server) ||
        !read_string(body, body_offset, record.broker_key.account.server) ||
        !read_string(body, body_offset, record.operation_key.account.server) ||
        !read_u64(body, body_offset, record.account.login) ||
        !read_u64(body, body_offset, record.broker_key.account.login) ||
        !read_u64(body, body_offset, record.broker_key.deal_ticket) ||
        !read_u64(body, body_offset, record.operation_key.account.login) ||
        !read_u64(body, body_offset, record.operation_key.trade_id) ||
        !read_u64(body, body_offset, record.operation_key.operation_id) ||
        !read_provenance(body, body_offset, record.broker_provenance) ||
        !read_volume(body, body_offset, record.managed_close_leg) ||
        !read_volume(body, body_offset, record.managed_reverse_open_leg) ||
        !read_volume(body, body_offset, record.unallocated_close_leg) ||
        !read_volume(body, body_offset, record.unallocated_reverse_open_leg) ||
        body_offset != body.size())
        return std::nullopt;
    return record.valid() ? std::optional<ManagedAllocationRecord>(std::move(record))
                          : std::nullopt;
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
        error = "managed allocation record has an invalid size";
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

ReadStatus read_record_file(const std::filesystem::path &path,
                            std::optional<ManagedAllocationRecord> &record,
                            std::string &error) {
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, bytes, error);
    if (status != ReadStatus::valid)
        return status;
    record = deserialize_record(bytes);
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

std::filesystem::path record_path(const std::filesystem::path &directory,
                                  const BrokerReversalKey &key) {
    const std::string suffix = "alloc-" + hex_u64(fnv1a(key.account.server)) + "-" +
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
    const ManagedAllocationRecord &record,
    const DurableBrokerReversalStore &broker_store) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "managed allocation store is unavailable");
        return ManagedAllocationCommitStatus::io_error;
    }
    const auto proof = validate_managed_allocation_proof(record, broker_store);
    if (proof != ManagedAllocationProofStatus::valid) {
        switch (proof) {
        case ManagedAllocationProofStatus::invalid_record:
            set_error(last_error_, "invalid managed allocation record");
            return ManagedAllocationCommitStatus::invalid_record;
        case ManagedAllocationProofStatus::missing_broker_record:
            set_error(last_error_, "durable broker reversal record is missing");
            return ManagedAllocationCommitStatus::missing_broker_record;
        case ManagedAllocationProofStatus::invalid_broker_record:
            set_error(last_error_, "durable broker reversal proof does not match");
            return ManagedAllocationCommitStatus::invalid_broker_record;
        case ManagedAllocationProofStatus::io_error:
            set_error(last_error_, "could not load durable broker reversal proof");
            return ManagedAllocationCommitStatus::io_error;
        case ManagedAllocationProofStatus::valid:
            break;
        }
    }
    const auto bytes = serialize_record(record);
    if (!bytes) {
        set_error(last_error_, "managed allocation record exceeds storage limits");
        return ManagedAllocationCommitStatus::invalid_record;
    }

    FileLock lock(directory_ / L".managed-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return ManagedAllocationCommitStatus::io_error;
    }
    const auto target = record_path(directory_, record.key());
    std::optional<ManagedAllocationRecord> existing;
    std::string read_error;
    const auto status = read_record_file(target, existing, read_error);
    if (status == ReadStatus::malformed) {
        set_error(last_error_, "malformed managed allocation record");
        return ManagedAllocationCommitStatus::invalid_record;
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, read_error);
        return ManagedAllocationCommitStatus::io_error;
    }
    if (status == ReadStatus::valid && existing) {
        if (*existing == record)
            return ManagedAllocationCommitStatus::already_committed;
        return ManagedAllocationCommitStatus::conflict;
    }

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
    (void)record;
    (void)broker_store;
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
    return ManagedAllocationCommitStatus::io_error;
#endif
}

ManagedAllocationLoadResult WindowsFileManagedAllocationStore::load(
    const BrokerReversalKey &key) const {
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
    std::optional<ManagedAllocationRecord> record;
    std::string error;
    const auto status = read_record_file(record_path(directory_, key), record, error);
    if (status == ReadStatus::valid && record && record->key() == key)
        return {ManagedAllocationLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed managed allocation record" : error);
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

ManagedAllocationScanResult WindowsFileManagedAllocationStore::scan() const {
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

    std::vector<ManagedAllocationRecord> records;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" ||
                filename.rfind(L"alloc-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                set_error(last_error_, "managed allocation record is not a regular file");
                return {ManagedAllocationScanStatus::invalid_record, {}};
            }
            std::optional<ManagedAllocationRecord> record;
            std::string error;
            const auto status = read_record_file(entry.path(), record, error);
            if (status == ReadStatus::malformed) {
                set_error(last_error_, error.empty() ? "malformed managed allocation record"
                                                      : error);
                return {ManagedAllocationScanStatus::invalid_record, {}};
            }
            if (status != ReadStatus::valid || !record) {
                set_error(last_error_, error.empty() ? "could not read managed allocation record"
                                                      : error);
                return {ManagedAllocationScanStatus::io_error, {}};
            }
            if (record_path(directory_, record->key()).filename() != entry.path().filename() ||
                !record->valid()) {
                set_error(last_error_, "managed allocation identity or metadata mismatch");
                return {ManagedAllocationScanStatus::invalid_record, {}};
            }
            records.push_back(std::move(*record));
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {ManagedAllocationScanStatus::io_error, {}};
    }

    std::sort(records.begin(), records.end(),
              [](const ManagedAllocationRecord &left,
                 const ManagedAllocationRecord &right) {
                  return left.key() < right.key();
              });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].key() == records[index].key()) {
            set_error(last_error_, "duplicate managed allocation key");
            return {ManagedAllocationScanStatus::invalid_record, {}};
        }
    }
    return {ManagedAllocationScanStatus::complete, std::move(records)};
#else
    set_error(last_error_, "WindowsFileManagedAllocationStore requires Windows");
    return {ManagedAllocationScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

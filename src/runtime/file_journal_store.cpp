/// \file file_journal_store.cpp
/// \brief Implements the Windows-backed durable operation journal store.

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

#include <mt5bridge/dispatch/file_journal_store.hpp>

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'J', 'N', 'L', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaxStringBytes = 1U << 20;
constexpr std::size_t kMaxPayloadBytes = 16U << 20;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes = 2U * (kMaxPayloadBytes + kMaxStringBytes) + 256U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

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

bool append_bytes(std::vector<std::uint8_t> &bytes, const std::string &value) {
    if (value.size() > kMaxStringBytes ||
        value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool append_payload(std::vector<std::uint8_t> &bytes,
                    const std::vector<std::uint8_t> &value) {
    if (value.size() > kMaxPayloadBytes ||
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

bool read_payload(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                  std::vector<std::uint8_t> &value) {
    std::uint32_t size = 0;
    if (!read_u32(bytes, offset, size) || size > kMaxPayloadBytes ||
        offset > bytes.size() || bytes.size() - offset < size)
        return false;
    value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                 bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return true;
}

std::uint64_t checksum(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> serialize_body(const OperationRecord &record) {
    std::vector<std::uint8_t> body;
    body.reserve(128 + record.key.account.server.size() + record.request_payload.size() +
                 record.result_payload.size());
    if (!append_bytes(body, record.key.account.server) ||
        !append_payload(body, record.request_payload) ||
        !append_payload(body, record.result_payload))
        return std::nullopt;
    append_u64(body, record.key.account.login);
    append_u64(body, record.key.trade_id);
    append_u64(body, record.key.operation_id);
    append_u32(body, static_cast<std::uint32_t>(record.operation_state));
    append_u32(body, static_cast<std::uint32_t>(record.journal_state));
    append_u64(body, record.revision);
    append_u64(body, record.fencing_token);
    return body;
}

std::optional<std::vector<std::uint8_t>> serialize_record(const OperationRecord &record) {
    const auto body = serialize_body(record);
    if (!body || body->size() > kMaxBodyBytes)
        return std::nullopt;

    std::vector<std::uint8_t> result;
    result.reserve(kMagic.size() + sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t) +
                   body->size());
    result.insert(result.end(), kMagic.begin(), kMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body->size()));
    append_u64(result, checksum(*body));
    result.insert(result.end(), body->begin(), body->end());
    return result;
}

std::optional<OperationRecord> deserialize_record(const std::vector<std::uint8_t> &bytes) {
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

    OperationRecord record;
    std::size_t body_offset = 0;
    if (!read_string(body, body_offset, record.key.account.server) ||
        !read_payload(body, body_offset, record.request_payload) ||
        !read_payload(body, body_offset, record.result_payload) ||
        !read_u64(body, body_offset, record.key.account.login) ||
        !read_u64(body, body_offset, record.key.trade_id) ||
        !read_u64(body, body_offset, record.key.operation_id))
        return std::nullopt;

    std::uint32_t operation_state = 0;
    std::uint32_t journal_state = 0;
    if (!read_u32(body, body_offset, operation_state) ||
        !read_u32(body, body_offset, journal_state) ||
        !read_u64(body, body_offset, record.revision) ||
        !read_u64(body, body_offset, record.fencing_token) ||
        body_offset != body.size())
        return std::nullopt;
    record.operation_state = static_cast<OperationState>(operation_state);
    record.journal_state = static_cast<JournalState>(journal_state);
    return record.valid() ? std::optional<OperationRecord>(std::move(record)) : std::nullopt;
}

#if defined(_WIN32)

std::string win32_error(const char *operation, DWORD error = GetLastError()) {
    return std::string(operation) + " failed (Win32 error " + std::to_string(error) + ")";
}

class FileLock final {
public:
    explicit FileLock(const std::filesystem::path &path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

ReadStatus read_file(const std::filesystem::path &path,
                     std::optional<OperationRecord> &record,
                     std::string &error) {
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
    if (!GetFileSizeEx(handle, &size)) {
        error = win32_error("GetFileSizeEx");
        CloseHandle(handle);
        return ReadStatus::io_error;
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxRecordBytes) {
        error = "journal record exceeds storage limits";
        CloseHandle(handle);
        return ReadStatus::malformed;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
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
                                  const OperationKey &key) {
    const std::string suffix = "op-" + hex_u64(fnv1a(key.account.server)) + "-" +
                               hex_u64(key.account.login) + "-" + hex_u64(key.trade_id) + "-" +
                               hex_u64(key.operation_id) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(suffix);
#else
    return directory / suffix;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

} // namespace

WindowsFileJournalStore::WindowsFileJournalStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "journal directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "journal path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    (void)directory_;
    set_error(last_error_, "WindowsFileJournalStore requires Windows");
#endif
}

StoreCommitStatus WindowsFileJournalStore::commit(
    const OperationRecord &record, std::optional<std::uint64_t> expected_revision) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !record.valid()) {
        set_error(last_error_, ready_ ? "invalid operation record" : "journal store is unavailable");
        return StoreCommitStatus::io_error;
    }
    if ((!expected_revision && record.revision != 1) ||
        (expected_revision &&
         (*expected_revision == (std::numeric_limits<std::uint64_t>::max)() ||
          record.revision != *expected_revision + 1))) {
        set_error(last_error_, "record revision does not follow compare-and-commit expectation");
        return StoreCommitStatus::io_error;
    }

    const auto bytes = serialize_record(record);
    if (!bytes) {
        set_error(last_error_, "operation record exceeds the storage limits");
        return StoreCommitStatus::io_error;
    }

    FileLock lock(directory_ / L".journal.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return StoreCommitStatus::io_error;
    }

    const auto target = record_path(directory_, record.key);
    std::optional<OperationRecord> existing;
    std::string read_error;
    const ReadStatus status = read_file(target, existing, read_error);
    if (status == ReadStatus::malformed || status == ReadStatus::io_error) {
        set_error(last_error_, status == ReadStatus::malformed ? "malformed journal record"
                                                                 : read_error);
        return StoreCommitStatus::io_error;
    }
    if (!expected_revision) {
        if (status == ReadStatus::valid)
            return StoreCommitStatus::conflict;
    } else if (status != ReadStatus::valid || !existing || existing->key != record.key ||
               existing->revision != *expected_revision) {
        return StoreCommitStatus::conflict;
    }

    const auto temp = target.wstring() + L".tmp";
    const std::filesystem::path temp_path(temp);
    DeleteFileW(temp_path.c_str());
    if (!write_file(temp_path, *bytes, last_error_))
        return StoreCommitStatus::io_error;
    if (!MoveFileExW(temp_path.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = win32_error("MoveFileExW");
        DeleteFileW(temp_path.c_str());
        return StoreCommitStatus::io_error;
    }
    return StoreCommitStatus::committed;
#else
    (void)record;
    (void)expected_revision;
    set_error(last_error_, "WindowsFileJournalStore requires Windows");
    return StoreCommitStatus::io_error;
#endif
}

StoreLoadResult WindowsFileJournalStore::load(const OperationKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !key.valid()) {
        set_error(last_error_, ready_ ? "invalid operation key" : "journal store is unavailable");
        return {key.valid() ? StoreLoadStatus::io_error : StoreLoadStatus::invalid_record,
                std::nullopt};
    }

    FileLock lock(directory_ / L".journal.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {StoreLoadStatus::io_error, std::nullopt};
    }
    std::optional<OperationRecord> record;
    std::string error;
    const ReadStatus status = read_file(record_path(directory_, key), record, error);
    if (status == ReadStatus::valid && record && record->key == key)
        return {StoreLoadStatus::found, std::move(record)};
    if (status == ReadStatus::malformed) {
        set_error(last_error_, error.empty() ? "malformed journal record" : error);
        return {StoreLoadStatus::invalid_record, std::nullopt};
    }
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {StoreLoadStatus::io_error, std::nullopt};
    }
    if (status == ReadStatus::valid) {
        set_error(last_error_, "journal filename collision");
        return {StoreLoadStatus::invalid_record, std::nullopt};
    }
    return {StoreLoadStatus::not_found, std::nullopt};
#else
    (void)key;
    set_error(last_error_, "WindowsFileJournalStore requires Windows");
    return {StoreLoadStatus::io_error, std::nullopt};
#endif
}

StoreScanResult WindowsFileJournalStore::scan() const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_) {
        set_error(last_error_, "journal store is unavailable");
        return {StoreScanStatus::io_error, {}};
    }

    FileLock lock(directory_ / L".journal.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {StoreScanStatus::io_error, {}};
    }

    std::vector<OperationRecord> records;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" ||
                filename.rfind(L"op-", 0) != 0)
                continue;
            if (!entry.is_regular_file()) {
                set_error(last_error_, "journal record is not a regular file");
                return {StoreScanStatus::invalid_record, {}};
            }
            std::optional<OperationRecord> record;
            std::string error;
            const auto status = read_file(entry.path(), record, error);
            if (status == ReadStatus::malformed) {
                set_error(last_error_, error.empty() ? "malformed journal record" : error);
                return {StoreScanStatus::invalid_record, {}};
            }
            if (status != ReadStatus::valid || !record) {
                set_error(last_error_, error.empty() ? "could not read journal record" : error);
                return {StoreScanStatus::io_error, {}};
            }
            if (record_path(directory_, record->key).filename() != entry.path().filename() ||
                !record->valid()) {
                set_error(last_error_, "journal record identity or metadata mismatch");
                return {StoreScanStatus::invalid_record, {}};
            }
            records.push_back(std::move(*record));
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {StoreScanStatus::io_error, {}};
    }

    std::sort(records.begin(), records.end(),
              [](const OperationRecord &left, const OperationRecord &right) {
                  return left.key < right.key;
              });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].key == records[index].key) {
            set_error(last_error_, "duplicate journal operation key");
            return {StoreScanStatus::invalid_record, {}};
        }
    }
    return {StoreScanStatus::complete, std::move(records)};
#else
    set_error(last_error_, "WindowsFileJournalStore requires Windows");
    return {StoreScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

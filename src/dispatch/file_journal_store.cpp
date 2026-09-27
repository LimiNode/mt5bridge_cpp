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
constexpr std::uint32_t kLegacyFormatVersion = 1;
constexpr std::uint32_t kFormatVersion = 2;
constexpr std::array<char, 8> kEpochMagic{{'M', 'T', '5', 'E', 'P', 'C', '0', '1'}};
constexpr std::uint32_t kEpochFormatVersion = 1;
constexpr std::size_t kMaxStringBytes = 1U << 20;
constexpr std::size_t kMaxPayloadBytes = 16U << 20;
constexpr std::uint32_t kMaxReconciliationPredicates = 1U << 20;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes = 2U * (kMaxPayloadBytes + kMaxStringBytes) + 256U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;
constexpr std::size_t kEpochEnvelopeBytes = kEpochMagic.size() + sizeof(std::uint32_t) +
                                             2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxEpochRecordBytes = kEpochEnvelopeBytes + kMaxStringBytes +
                                              sizeof(std::uint32_t) +
                                              2U * sizeof(std::uint64_t);

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

bool append_reconciliation_descriptor(
    std::vector<std::uint8_t> &bytes,
    const std::optional<ReconciliationDescriptor> &descriptor) {
    append_u32(bytes, descriptor ? 1u : 0u);
    if (!descriptor)
        return true;
    if (!descriptor->valid() ||
        descriptor->predicates.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    const auto &baseline = descriptor->baseline;
    if (!append_bytes(bytes, descriptor->account.server))
        return false;
    append_u64(bytes, descriptor->account.login);
    append_u64(bytes, descriptor->trade_id);
    append_u64(bytes, descriptor->operation_id);
    if (!append_bytes(bytes, baseline.account().server))
        return false;
    append_u64(bytes, baseline.account().login);
    append_u64(bytes, baseline.graph_instance_id());
    append_u64(bytes, baseline.graph_revision());
    append_u64(bytes, baseline.active_orders_revision());
    append_u64(bytes, baseline.positions_revision());
    append_u64(bytes, baseline.history_orders_revision());
    append_u64(bytes, baseline.history_deals_revision());
    append_u32(bytes, static_cast<std::uint32_t>(descriptor->settled_state));
    append_u32(bytes, static_cast<std::uint32_t>(descriptor->predicates.size()));
    for (const auto &predicate : descriptor->predicates) {
        append_u32(bytes, static_cast<std::uint32_t>(predicate.kind));
        append_u64(bytes, predicate.ticket);
        append_u32(bytes, predicate.history_window ? 1u : 0u);
        if (predicate.history_window) {
            append_u64(bytes, static_cast<std::uint64_t>(predicate.history_window->from_msc));
            append_u64(bytes, static_cast<std::uint64_t>(predicate.history_window->to_msc));
        }
        append_u32(bytes, predicate.baseline_present
                              ? (*predicate.baseline_present ? 2u : 1u)
                              : 0u);
        append_u64(bytes, predicate.correlation_id);
        append_u32(bytes, static_cast<std::uint32_t>(predicate.expected_transition));
    }
    return true;
}

bool append_reconciliation_bindings(
    std::vector<std::uint8_t> &bytes,
    const std::vector<ReconciliationBinding> &bindings) {
    if (bindings.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(bindings.size()));
    for (const auto &binding : bindings) {
        if (!binding.valid())
            return false;
        append_u64(bytes, binding.correlation_id);
        append_u64(bytes, binding.broker_ticket);
    }
    return true;
}

bool read_reconciliation_descriptor(
    const std::vector<std::uint8_t> &bytes, std::size_t &offset,
    std::optional<ReconciliationDescriptor> &descriptor, bool extended) {
    if (offset == bytes.size())
        return true;
    std::uint32_t present = 0;
    if (!read_u32(bytes, offset, present) || present > 1)
        return false;
    if (present == 0)
        return true;

    AccountKey descriptor_account;
    std::string baseline_server;
    std::uint64_t baseline_login = 0;
    std::uint64_t trade_id = 0;
    std::uint64_t operation_id = 0;
    std::uint64_t graph_instance_id = 0;
    std::uint64_t graph_revision = 0;
    std::uint64_t active_orders_revision = 0;
    std::uint64_t positions_revision = 0;
    std::uint64_t history_orders_revision = 0;
    std::uint64_t history_deals_revision = 0;
    std::uint32_t settled_state = 0;
    std::uint32_t predicate_count = 0;
    if (!read_string(bytes, offset, descriptor_account.server) ||
        !read_u64(bytes, offset, descriptor_account.login) ||
        (extended && !read_u64(bytes, offset, trade_id)) ||
        (extended && !read_u64(bytes, offset, operation_id)) ||
        !read_string(bytes, offset, baseline_server) ||
        !read_u64(bytes, offset, baseline_login) ||
        !read_u64(bytes, offset, graph_instance_id) ||
        !read_u64(bytes, offset, graph_revision) ||
        !read_u64(bytes, offset, active_orders_revision) ||
        !read_u64(bytes, offset, positions_revision) ||
        !read_u64(bytes, offset, history_orders_revision) ||
        !read_u64(bytes, offset, history_deals_revision) ||
        !read_u32(bytes, offset, settled_state) ||
        !read_u32(bytes, offset, predicate_count) ||
        predicate_count > kMaxReconciliationPredicates)
        return false;
    const AccountKey baseline_account{std::move(baseline_server), baseline_login};
    const auto baseline = ReconciliationBaseline::restore(
        baseline_account, graph_instance_id, graph_revision, active_orders_revision,
        positions_revision, history_orders_revision, history_deals_revision);
    if (!baseline)
        return false;
    ReconciliationDescriptor value{std::move(descriptor_account), *baseline, {},
                                   static_cast<OperationState>(settled_state), trade_id,
                                   operation_id};
    value.predicates.reserve(predicate_count);
    for (std::uint32_t index = 0; index < predicate_count; ++index) {
        std::uint32_t kind = 0;
        std::uint64_t ticket = 0;
        std::uint32_t has_window = 0;
        if (!read_u32(bytes, offset, kind) || !read_u64(bytes, offset, ticket) ||
            !read_u32(bytes, offset, has_window) || has_window > 1)
            return false;
        ReconciliationPredicate predicate;
        predicate.kind = static_cast<ReconciliationPredicateKind>(kind);
        predicate.ticket = ticket;
        if (has_window != 0) {
            std::uint64_t from = 0;
            std::uint64_t to = 0;
            if (!read_u64(bytes, offset, from) || !read_u64(bytes, offset, to))
                return false;
            predicate.history_window = ObservationWindow{
                static_cast<std::int64_t>(from), static_cast<std::int64_t>(to)};
        }
        if (extended) {
            std::uint32_t baseline_state = 0;
            std::uint32_t transition = 0;
            if (!read_u32(bytes, offset, baseline_state) || baseline_state > 2 ||
                !read_u64(bytes, offset, predicate.correlation_id) ||
                !read_u32(bytes, offset, transition))
                return false;
            if (baseline_state != 0)
                predicate.baseline_present = baseline_state == 2;
            predicate.expected_transition =
                static_cast<ReconciliationTransition>(transition);
        } else {
            // Version-one descriptors predate the causal metadata. Preserve
            // only their pre-dispatch compatibility: infer the conservative
            // default transition from the predicate kind, while leaving the
            // descriptor operation ids unset so a post-dispatch record still
            // fails closed during OperationRecord::valid().
            const bool absence =
                predicate.kind == ReconciliationPredicateKind::active_order_absent ||
                predicate.kind == ReconciliationPredicateKind::position_absent ||
                predicate.kind == ReconciliationPredicateKind::history_order_absent ||
                predicate.kind == ReconciliationPredicateKind::history_deal_absent;
            predicate = with_reconciliation_transition(std::move(predicate), absence);
        }
        value.predicates.push_back(std::move(predicate));
    }
    if (!value.valid())
        return false;
    descriptor = std::move(value);
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
    if (!append_reconciliation_descriptor(body, record.reconciliation_descriptor))
        return std::nullopt;
    if (!append_reconciliation_bindings(body, record.reconciliation_bindings))
        return std::nullopt;
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
        !read_u64(bytes, offset, expected_checksum) ||
        (version != kFormatVersion && version != kLegacyFormatVersion) ||
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
        !read_reconciliation_descriptor(body, body_offset,
                                        record.reconciliation_descriptor,
                                        version >= kFormatVersion))
        return std::nullopt;
    if (version >= kFormatVersion) {
        std::uint32_t binding_count = 0;
        if (!read_u32(body, body_offset, binding_count) ||
            binding_count > kMaxReconciliationPredicates)
            return std::nullopt;
        record.reconciliation_bindings.reserve(binding_count);
        for (std::uint32_t index = 0; index < binding_count; ++index) {
            ReconciliationBinding binding;
            if (!read_u64(body, body_offset, binding.correlation_id) ||
                !read_u64(body, body_offset, binding.broker_ticket) ||
                !binding.valid())
                return std::nullopt;
            record.reconciliation_bindings.push_back(binding);
        }
    }
    if (body_offset != body.size())
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

std::string hex_bytes(const std::string &value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::filesystem::path lease_path(const std::filesystem::path &directory,
                                 const AccountKey &account, const char *suffix) {
    const std::string filename = "lease-" + hex_bytes(account.server) + "-" +
                                 hex_u64(account.login);
    return directory / widen_ascii(filename + suffix);
}

std::optional<std::vector<std::uint8_t>> serialize_epoch(const AccountKey &account,
                                                         std::uint64_t epoch) {
    if (!account.valid() || epoch == 0)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_bytes(body, account.server))
        return std::nullopt;
    append_u64(body, account.login);
    append_u64(body, epoch);

    std::vector<std::uint8_t> result;
    result.insert(result.end(), kEpochMagic.begin(), kEpochMagic.end());
    append_u32(result, kEpochFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body.size()));
    append_u64(result, checksum(body));
    result.insert(result.end(), body.begin(), body.end());
    return result.size() <= kMaxEpochRecordBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(result))
               : std::nullopt;
}

bool deserialize_epoch(const std::vector<std::uint8_t> &bytes,
                       const AccountKey &expected_account, std::uint64_t &epoch,
                       std::string &error) {
    if (bytes.size() < kEpochEnvelopeBytes || bytes.size() > kMaxEpochRecordBytes ||
        !std::equal(kEpochMagic.begin(), kEpochMagic.end(), bytes.begin())) {
        error = "fencing epoch envelope is malformed";
        return false;
    }
    std::size_t offset = kEpochMagic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, offset, version) || !read_u64(bytes, offset, body_size) ||
        !read_u64(bytes, offset, expected_checksum) || version != kEpochFormatVersion ||
        body_size != bytes.size() - offset) {
        error = "fencing epoch envelope is malformed";
        return false;
    }
    const std::vector<std::uint8_t> body(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    if (checksum(body) != expected_checksum) {
        error = "fencing epoch checksum mismatch";
        return false;
    }
    std::size_t body_offset = 0;
    AccountKey account;
    if (!read_string(body, body_offset, account.server) ||
        !read_u64(body, body_offset, account.login) ||
        !read_u64(body, body_offset, epoch) || body_offset != body.size() ||
        account != expected_account || epoch == 0) {
        error = "fencing epoch identity or value is malformed";
        return false;
    }
    return true;
}

bool read_epoch(const std::filesystem::path &path, const AccountKey &account,
                std::uint64_t &epoch, std::string &error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            epoch = 0;
            return true;
        }
        error = win32_error("CreateFileW(epoch)", code);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxEpochRecordBytes) {
        error = size.QuadPart < 0 ? "fencing epoch has an invalid size"
                                  : "fencing epoch is malformed";
        CloseHandle(handle);
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data() + offset, requested, &read, nullptr) || read == 0) {
            error = win32_error("ReadFile(epoch)");
            CloseHandle(handle);
            return false;
        }
        offset += read;
    }
    CloseHandle(handle);
    return deserialize_epoch(bytes, account, epoch, error);
}

bool write_epoch(const std::filesystem::path &path, const AccountKey &account,
                 std::uint64_t epoch, std::string &error) {
    const auto bytes = serialize_epoch(account, epoch);
    if (!bytes) {
        error = "fencing epoch cannot be serialized";
        return false;
    }
    const std::filesystem::path temporary(path.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, error))
        return false;
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = win32_error("MoveFileExW(epoch)");
        DeleteFileW(temporary.c_str());
        return false;
    }
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

#if defined(_WIN32)
struct WindowsSingleWriterLease::State {
    HANDLE lock = INVALID_HANDLE_VALUE;

    ~State() {
        if (lock != INVALID_HANDLE_VALUE)
            CloseHandle(lock);
    }
};
#else
struct WindowsSingleWriterLease::State {};
#endif

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

WindowsSingleWriterLease::WindowsSingleWriterLease(std::filesystem::path directory,
                                                   AccountKey account)
    : directory_(std::move(directory)), account_(std::move(account)) {
#if defined(_WIN32)
    try {
        const auto fail = [this](std::string message) {
            set_error(last_error_, std::move(message));
            ready_ = false;
            token_ = 0;
            state_.reset();
        };
        if (!account_.valid()) {
            set_error(last_error_, "invalid lease account");
            return;
        }
        if (directory_.empty()) {
            set_error(last_error_, "lease directory is empty");
            return;
        }
        const auto absolute_directory = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute_directory);
        if (!std::filesystem::is_directory(absolute_directory)) {
            set_error(last_error_, "lease path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute_directory);
        state_ = std::make_unique<State>();
        const auto lock = lease_path(directory_, account_, ".lock");
        state_->lock = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (state_->lock == INVALID_HANDLE_VALUE) {
            fail(win32_error("CreateFileW(lease lock)"));
            return;
        }

        const auto epoch = lease_path(directory_, account_, ".epoch");
        std::uint64_t previous = 0;
        if (!read_epoch(epoch, account_, previous, last_error_)) {
            const auto error = last_error_;
            fail(error);
            return;
        }
        if (previous == (std::numeric_limits<std::uint64_t>::max)()) {
            fail("fencing epoch exhausted");
            return;
        }
        token_ = previous + 1;
        if (!write_epoch(epoch, account_, token_, last_error_)) {
            const auto error = last_error_;
            fail(error);
            return;
        }
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        state_.reset();
        token_ = 0;
    } catch (const std::exception &error) {
        set_error(last_error_, error.what());
        state_.reset();
        token_ = 0;
    }
#else
    (void)directory_;
    (void)account_;
    set_error(last_error_, "WindowsSingleWriterLease requires Windows");
#endif
}

WindowsSingleWriterLease::~WindowsSingleWriterLease() = default;

std::optional<std::uint64_t> WindowsSingleWriterLease::held_fencing_token(
    const AccountKey &account) const {
    if (!ready_ || token_ == 0 || account != account_)
        return std::nullopt;
    return token_;
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

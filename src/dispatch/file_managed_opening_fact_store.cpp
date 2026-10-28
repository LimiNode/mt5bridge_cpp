/// \file file_managed_opening_fact_store.cpp
/// \brief Implements the Windows-backed durable OPEN fact store.

#include "managed_opening_fact_store.hpp"

#include "file_store_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge::dispatch {
namespace {

using namespace file_store_support;

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'O', 'P', 'N', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxBodyBytes =
    kManagedOpeningMaxCanonicalBytes + kMaxStringBytes + 64U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

enum class RecordReadStatus { absent, valid, malformed, io_error };

/// \brief Builds an unambiguous binary key for an account namespace.
/// \param account Account namespace.
/// \return Length-delimited server and fixed-width login bytes.
std::string account_key_bytes(const AccountKey &account) {
    std::vector<std::uint8_t> bytes;
    append_u64(bytes, static_cast<std::uint64_t>(account.server.size()));
    bytes.insert(bytes.end(), account.server.begin(), account.server.end());
    append_u64(bytes, account.login);
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/// \brief Builds an unambiguous binary key for one account/deal pair.
/// \param account Account namespace.
/// \param deal_ticket Stable broker execution identity.
/// \return Account key bytes followed by fixed-width deal ticket bytes.
std::string record_key_bytes(const AccountKey &account, std::uint64_t deal_ticket) {
    auto bytes = account_key_bytes(account);
    std::vector<std::uint8_t> suffix;
    append_u64(suffix, deal_ticket);
    bytes.append(reinterpret_cast<const char *>(suffix.data()), suffix.size());
    return bytes;
}

/// \brief Maps an account namespace to its isolated directory.
/// \param directory Store root directory.
/// \param account Account namespace.
/// \return Deterministic account directory path.
std::filesystem::path account_directory(const std::filesystem::path &directory,
                                        const AccountKey &account) {
    return directory /
           widen_ascii("account-" + hex_u64(fnv1a(account_key_bytes(account))));
}

/// \brief Maps one account/deal key to its bounded record filename.
/// \param directory Store directory.
/// \param account Account namespace.
/// \param deal_ticket Stable broker execution identity.
/// \return Deterministic record path.
std::filesystem::path record_path(const std::filesystem::path &directory,
                                  const AccountKey &account,
                                  std::uint64_t deal_ticket) {
    return account_directory(directory, account) /
           widen_ascii("open-" + hex_u64(fnv1a(record_key_bytes(account, deal_ticket))) +
                       ".bin");
}

/// \brief Encodes one fact inside the versioned durable envelope.
/// \param fact Locally validated fact.
/// \return Complete record bytes, or empty for an invalid/oversized fact.
std::optional<std::vector<std::uint8_t>> serialize_record(
    const ManagedOpeningDealFact &fact) {
    if (!fact.valid())
        return std::nullopt;
    const auto canonical = fact.canonical_bytes();
    std::vector<std::uint8_t> body;
    if (!append_string(body, fact.operation_key().account.server) ||
        body.size() > kMaxBodyBytes - sizeof(std::uint64_t) * 2U ||
        !append_payload(body, canonical, kManagedOpeningMaxCanonicalBytes))
        return std::nullopt;
    append_u64(body, fact.operation_key().account.login);
    append_u64(body, fact.deal_ticket());

    std::vector<std::uint8_t> result;
    result.reserve(kEnvelopeBytes + body.size());
    result.insert(result.end(), kMagic.begin(), kMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body.size()));
    append_u64(result, checksum(body));
    result.insert(result.end(), body.begin(), body.end());
    return result.size() <= kMaxRecordBytes
               ? std::optional<std::vector<std::uint8_t>>(std::move(result))
               : std::nullopt;
}

/// \brief Reads and validates one complete durable fact record.
/// \param path Record path.
/// \param[out] fact Decoded fact when valid.
/// \param[out] error Diagnostic for malformed or failed reads.
/// \return Classified durable read result.
RecordReadStatus read_record(const std::filesystem::path &path,
                             std::optional<ManagedOpeningDealFact> &fact,
                             std::string &error) {
    fact.reset();
#if defined(_WIN32)
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, kMaxRecordBytes, bytes, error,
                                        ReadStatus::malformed);
    if (status == ReadStatus::absent)
        return RecordReadStatus::absent;
    if (status == ReadStatus::io_error)
        return RecordReadStatus::io_error;
    if (bytes.size() < kEnvelopeBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        error = "managed opening fact has an invalid envelope";
        return RecordReadStatus::malformed;
    }
    std::size_t offset = kMagic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, offset, version) || !read_u64(bytes, offset, body_size) ||
        !read_u64(bytes, offset, expected_checksum) || version != kFormatVersion ||
        body_size > kMaxBodyBytes || body_size != bytes.size() - offset) {
        error = "managed opening fact has an unsupported or truncated envelope";
        return RecordReadStatus::malformed;
    }
    const std::vector<std::uint8_t> body(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    if (checksum(body) != expected_checksum) {
        error = "managed opening fact checksum mismatch";
        return RecordReadStatus::malformed;
    }

    std::size_t body_offset = 0;
    std::string account_server;
    std::uint64_t account_login = 0;
    std::uint64_t deal_ticket = 0;
    std::vector<std::uint8_t> canonical;
    if (!read_string(body, body_offset, account_server) ||
        !read_payload(body, body_offset, canonical,
                      kManagedOpeningMaxCanonicalBytes) ||
        !read_u64(body, body_offset, account_login) ||
        !read_u64(body, body_offset, deal_ticket) || body_offset != body.size()) {
        error = "managed opening fact body is malformed";
        return RecordReadStatus::malformed;
    }
    auto decoded = ManagedOpeningDealFact::from_canonical_bytes(canonical);
    if (!decoded || decoded->operation_key().account.server != account_server ||
        decoded->operation_key().account.login != account_login ||
        decoded->deal_ticket() != deal_ticket) {
        error = "managed opening fact identity does not match its envelope";
        return RecordReadStatus::malformed;
    }
    fact = std::move(decoded);
    return RecordReadStatus::valid;
#else
    (void)path;
    (void)fact;
    error = "WindowsFileManagedOpeningFactStore requires Windows";
    return RecordReadStatus::io_error;
#endif
}

bool matches_requested_identity(const ManagedOpeningDealFact &fact,
                                const AccountKey &account,
                                std::uint64_t deal_ticket) {
    return fact.operation_key().account == account && fact.deal_ticket() == deal_ticket;
}

} // namespace

WindowsFileManagedOpeningFactStore::WindowsFileManagedOpeningFactStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            last_error_ = "managed opening fact directory is empty";
            return;
        }
        const auto absolute = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute);
        if (!std::filesystem::is_directory(absolute)) {
            last_error_ = "managed opening fact path is not a directory";
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        last_error_ = error.what();
    }
#else
    (void)directory_;
    last_error_ = "WindowsFileManagedOpeningFactStore requires Windows";
#endif
}

ManagedOpeningFactCommitStatus WindowsFileManagedOpeningFactStore::commit(
    const ManagedOpeningDealFact &fact, const SingleWriterLease &lease) {
    last_error_.clear();
    if (!ready_ || !fact.valid())
        return ManagedOpeningFactCommitStatus::invalid_record;
    const auto token = lease.held_fencing_token(fact.operation_key().account);
    if (!token)
        return ManagedOpeningFactCommitStatus::lease_not_held;

    ManagedOpeningFactCommitStatus result = ManagedOpeningFactCommitStatus::io_error;
    const bool fenced = lease.with_fencing_capability(
        fact.operation_key().account, *token, [&] {
#if defined(_WIN32)
            FileLock lock(directory_ / L"managed-opening-facts.lock");
            if (!lock.acquired()) {
                last_error_ = lock.error();
                result = ManagedOpeningFactCommitStatus::io_error;
                return;
            }
            const auto target = record_path(directory_, fact.operation_key().account,
                                            fact.deal_ticket());
            try {
                std::filesystem::create_directories(target.parent_path());
                if (!std::filesystem::is_directory(target.parent_path())) {
                    last_error_ = "managed opening account path is not a directory";
                    result = ManagedOpeningFactCommitStatus::io_error;
                    return;
                }
            } catch (const std::filesystem::filesystem_error &error) {
                last_error_ = error.what();
                result = ManagedOpeningFactCommitStatus::io_error;
                return;
            }
            std::optional<ManagedOpeningDealFact> existing;
            const auto read_status = read_record(target, existing, last_error_);
            if (read_status == RecordReadStatus::valid) {
                if (!matches_requested_identity(*existing, fact.operation_key().account,
                                                fact.deal_ticket())) {
                    last_error_ = "managed opening fact identity collides with requested key";
                    result = ManagedOpeningFactCommitStatus::invalid_record;
                    return;
                }
                result = existing->same_execution_payload(fact)
                             ? ManagedOpeningFactCommitStatus::already_committed
                             : ManagedOpeningFactCommitStatus::conflict;
                return;
            }
            if (read_status == RecordReadStatus::malformed) {
                result = ManagedOpeningFactCommitStatus::invalid_record;
                return;
            }
            if (read_status == RecordReadStatus::io_error) {
                result = ManagedOpeningFactCommitStatus::io_error;
                return;
            }
            const auto bytes = serialize_record(fact);
            if (!bytes) {
                result = ManagedOpeningFactCommitStatus::invalid_record;
                return;
            }
            const auto temporary = std::filesystem::path(target.wstring() + L".tmp");
            if (!write_file(temporary, *bytes, last_error_) ||
                !MoveFileExW(temporary.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                if (last_error_.empty())
                    last_error_ = win32_error("MoveFileExW(managed opening fact)");
                result = ManagedOpeningFactCommitStatus::io_error;
                return;
            }
            result = ManagedOpeningFactCommitStatus::committed;
#else
            result = ManagedOpeningFactCommitStatus::io_error;
#endif
        });
    return fenced ? result : ManagedOpeningFactCommitStatus::lease_not_held;
}

ManagedOpeningFactLoadResult WindowsFileManagedOpeningFactStore::load(
    const AccountKey &account, std::uint64_t deal_ticket) const {
    last_error_.clear();
    if (!ready_ || !account.valid() || deal_ticket == 0)
        return {ManagedOpeningFactLoadStatus::invalid_record, std::nullopt};
    std::optional<ManagedOpeningDealFact> fact;
    std::string error;
    const auto target = record_path(directory_, account, deal_ticket);
    const auto status = read_record(target, fact, error);
    last_error_ = error;
    if (status == RecordReadStatus::absent)
        return {ManagedOpeningFactLoadStatus::not_found, std::nullopt};
    if (status == RecordReadStatus::malformed)
        return {ManagedOpeningFactLoadStatus::invalid_record, std::nullopt};
    if (status == RecordReadStatus::io_error)
        return {ManagedOpeningFactLoadStatus::io_error, std::nullopt};
    if (!matches_requested_identity(*fact, account, deal_ticket)) {
        last_error_ = "managed opening fact identity does not match requested lookup";
        return {ManagedOpeningFactLoadStatus::invalid_record, std::nullopt};
    }
    return {ManagedOpeningFactLoadStatus::found, std::move(fact)};
}

ManagedOpeningFactScanResult WindowsFileManagedOpeningFactStore::scan(
    const AccountKey &account) const {
    last_error_.clear();
    if (!ready_ || !account.valid())
        return {ManagedOpeningFactScanStatus::invalid_record, {}};
#if defined(_WIN32)
    try {
        const auto account_path = account_directory(directory_, account);
        std::error_code account_status_error;
        const auto account_status =
            std::filesystem::symlink_status(account_path, account_status_error);
        const bool no_such_account =
            account_status_error ==
                std::make_error_code(std::errc::no_such_file_or_directory) ||
            account_status_error.value() == ERROR_FILE_NOT_FOUND ||
            account_status_error.value() == ERROR_PATH_NOT_FOUND;
        const bool account_missing =
            (account_status.type() == std::filesystem::file_type::not_found ||
             no_such_account) &&
            (!account_status_error || no_such_account);
        if (account_missing)
            return {ManagedOpeningFactScanStatus::complete, {}};
        if (account_status_error) {
            last_error_ = account_status_error.message();
            return {ManagedOpeningFactScanStatus::io_error, {}};
        }
        if (std::filesystem::is_symlink(account_status) ||
            !std::filesystem::is_directory(account_status)) {
            last_error_ = "managed opening account path is not a directory";
            return {ManagedOpeningFactScanStatus::invalid_record, {}};
        }
        const auto account_attributes = GetFileAttributesW(account_path.c_str());
        if (account_attributes == INVALID_FILE_ATTRIBUTES) {
            last_error_ = win32_error("GetFileAttributesW(managed opening account)");
            return {ManagedOpeningFactScanStatus::io_error, {}};
        }
        if ((account_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            last_error_ = "managed opening account path is a reparse point";
            return {ManagedOpeningFactScanStatus::invalid_record, {}};
        }
        std::vector<ManagedOpeningDealFact> facts;
        for (const auto &entry : std::filesystem::directory_iterator(account_path)) {
            const auto filename = entry.path().filename().wstring();
            if (filename.rfind(L"open-", 0) != 0 ||
                entry.path().extension() != L".bin")
                continue;
            std::error_code entry_status_error;
            const auto entry_status = entry.symlink_status(entry_status_error);
            if (entry_status_error) {
                last_error_ = entry_status_error.message();
                return {ManagedOpeningFactScanStatus::io_error, {}};
            }
            if (std::filesystem::is_symlink(entry_status) ||
                !std::filesystem::is_regular_file(entry_status)) {
                last_error_ = "managed opening reserved path is not a regular file";
                return {ManagedOpeningFactScanStatus::invalid_record, {}};
            }
            const auto entry_attributes = GetFileAttributesW(entry.path().c_str());
            if (entry_attributes == INVALID_FILE_ATTRIBUTES) {
                last_error_ = win32_error("GetFileAttributesW(managed opening fact)");
                return {ManagedOpeningFactScanStatus::io_error, {}};
            }
            if ((entry_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                last_error_ = "managed opening reserved path is a reparse point";
                return {ManagedOpeningFactScanStatus::invalid_record, {}};
            }
            std::optional<ManagedOpeningDealFact> fact;
            std::string error;
            const auto read_status = read_record(entry.path(), fact, error);
            if (read_status != RecordReadStatus::valid) {
                last_error_ = error;
                return {read_status == RecordReadStatus::io_error
                            ? ManagedOpeningFactScanStatus::io_error
                            : ManagedOpeningFactScanStatus::invalid_record,
                        {}};
            }
            if (!matches_requested_identity(*fact, account, fact->deal_ticket())) {
                last_error_ = "managed opening fact account collision";
                return {ManagedOpeningFactScanStatus::invalid_record, {}};
            }
            if (record_path(directory_, account, fact->deal_ticket()).filename() !=
                entry.path().filename()) {
                last_error_ = "managed opening fact filename collision";
                return {ManagedOpeningFactScanStatus::invalid_record, {}};
            }
            facts.push_back(std::move(*fact));
        }
        std::sort(facts.begin(), facts.end(), [](const auto &left, const auto &right) {
            return left.deal_ticket() < right.deal_ticket();
        });
        for (std::size_t index = 1; index < facts.size(); ++index)
            if (facts[index - 1].deal_ticket() == facts[index].deal_ticket()) {
                last_error_ = "duplicate managed opening fact ticket";
                return {ManagedOpeningFactScanStatus::invalid_record, {}};
            }
        return {ManagedOpeningFactScanStatus::complete, std::move(facts)};
    } catch (const std::filesystem::filesystem_error &error) {
        last_error_ = error.what();
        return {ManagedOpeningFactScanStatus::io_error, {}};
    }
#else
    return {ManagedOpeningFactScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge::dispatch

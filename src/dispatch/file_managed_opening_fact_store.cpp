/// \file file_managed_opening_fact_store.cpp
/// \brief Implements the Windows-backed durable OPEN fact store.

#include "managed_opening_fact_store.hpp"

#include "file_store_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
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
constexpr std::size_t kMaxBodyBytes = kMaxPayloadBytes + 128U;
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

enum class RecordReadStatus { absent, valid, malformed, io_error };

/// \brief Builds the stable namespace input for a record filename.
/// \param account Account namespace.
/// \param deal_ticket Stable broker execution identity.
/// \return Length-delimited logical key text.
std::string key_text(const AccountKey &account, std::uint64_t deal_ticket) {
    return account.server + "\0" + std::to_string(account.login) + "\0" +
           std::to_string(deal_ticket);
}

/// \brief Maps one account/deal key to its bounded record filename.
/// \param directory Store directory.
/// \param account Account namespace.
/// \param deal_ticket Stable broker execution identity.
/// \return Deterministic record path.
std::filesystem::path record_path(const std::filesystem::path &directory,
                                  const AccountKey &account,
                                  std::uint64_t deal_ticket) {
    return directory / widen_ascii("open-" + hex_u64(fnv1a(key_text(account, deal_ticket))) +
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
        !append_payload(body, canonical, kMaxPayloadBytes))
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
        !read_payload(body, body_offset, canonical) ||
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
            std::optional<ManagedOpeningDealFact> existing;
            const auto read_status = read_record(target, existing, last_error_);
            if (read_status == RecordReadStatus::valid) {
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
    const auto status = read_record(record_path(directory_, account, deal_ticket), fact,
                                    error);
    last_error_ = error;
    if (status == RecordReadStatus::absent)
        return {ManagedOpeningFactLoadStatus::not_found, std::nullopt};
    if (status == RecordReadStatus::malformed)
        return {ManagedOpeningFactLoadStatus::invalid_record, std::nullopt};
    if (status == RecordReadStatus::io_error)
        return {ManagedOpeningFactLoadStatus::io_error, std::nullopt};
    return {ManagedOpeningFactLoadStatus::found, std::move(fact)};
}

ManagedOpeningFactScanResult WindowsFileManagedOpeningFactStore::scan(
    const AccountKey &account) const {
    last_error_.clear();
    if (!ready_ || !account.valid())
        return {ManagedOpeningFactScanStatus::invalid_record, {}};
#if defined(_WIN32)
    try {
        std::vector<ManagedOpeningDealFact> facts;
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            if (!entry.is_regular_file())
                continue;
            const auto filename = entry.path().filename().wstring();
            if (filename.rfind(L"open-", 0) != 0 ||
                entry.path().extension() != L".bin")
                continue;
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
            if (fact->operation_key().account != account)
                continue;
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

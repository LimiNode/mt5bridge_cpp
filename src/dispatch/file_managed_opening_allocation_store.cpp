/// \file file_managed_opening_allocation_store.cpp
/// \brief Implements durable source-derived OPEN allocation publication.

#include "managed_opening_allocation.hpp"

#include "managed_opening_fact_store.hpp"
#include "managed_opening_source_verifier.hpp"

#include "file_store_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
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

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'O', 'A', 'L', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaxFacts = 4096;
constexpr std::size_t kMaxBodyBytes = 1U << 20;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

enum class ReadStatus { absent, valid, malformed, io_error };

enum class TemporaryRecordStatus { valid, invalid_record, io_error };

/// \brief Validates an unfinished atomic-write artifact before ignoring it.
/// \param path Candidate `*.bin.tmp` path under the account namespace.
/// \param error Receives a fail-closed filesystem diagnostic.
/// \return Whether the artifact is a regular, non-reparse file that may be ignored.
TemporaryRecordStatus validate_temporary_record(const std::filesystem::path &path,
                                                std::string &error) {
#if defined(_WIN32)
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error) {
        error = status_error.message();
        return TemporaryRecordStatus::io_error;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        error = "managed opening allocation temporary is not a regular file";
        return TemporaryRecordStatus::invalid_record;
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = win32_error("GetFileAttributesW(managed opening allocation temporary)");
        return TemporaryRecordStatus::io_error;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "managed opening allocation temporary is a reparse point";
        return TemporaryRecordStatus::invalid_record;
    }
    return TemporaryRecordStatus::valid;
#else
    (void)path;
    error = "WindowsFileManagedOpeningAllocationStore requires Windows";
    return TemporaryRecordStatus::io_error;
#endif
}

std::string account_key_bytes(const AccountKey &account) {
    std::vector<std::uint8_t> bytes;
    append_u64(bytes, static_cast<std::uint64_t>(account.server.size()));
    bytes.insert(bytes.end(), account.server.begin(), account.server.end());
    append_u64(bytes, account.login);
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::string allocation_key_bytes(const ManagedOpeningAllocationKey &key) {
    std::vector<std::uint8_t> bytes;
    append_u64(bytes, static_cast<std::uint64_t>(key.operation_key.account.server.size()));
    bytes.insert(bytes.end(), key.operation_key.account.server.begin(),
                 key.operation_key.account.server.end());
    append_u64(bytes, key.operation_key.account.login);
    append_u64(bytes, key.operation_key.trade_id);
    append_u64(bytes, key.operation_key.operation_id);
    append_u64(bytes, key.operation_revision);
    append_u64(bytes, key.frontier_revision);
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::filesystem::path account_directory(const std::filesystem::path &directory,
                                        const AccountKey &account) {
    return directory /
           widen_ascii("account-" + hex_u64(fnv1a(account_key_bytes(account))));
}

std::filesystem::path record_path(const std::filesystem::path &directory,
                                  const ManagedOpeningAllocationKey &key,
                                  const char *prefix) {
    return account_directory(directory, key.operation_key.account) /
           widen_ascii(std::string(prefix) +
                       hex_u64(fnv1a(allocation_key_bytes(key))) + ".bin");
}

bool append_key(std::vector<std::uint8_t> &bytes,
                const ManagedOpeningAllocationKey &key) {
    if (!append_string(bytes, key.operation_key.account.server))
        return false;
    append_u64(bytes, key.operation_key.account.login);
    append_u64(bytes, key.operation_key.trade_id);
    append_u64(bytes, key.operation_key.operation_id);
    append_u64(bytes, key.operation_revision);
    append_u64(bytes, key.frontier_revision);
    return true;
}

bool read_key(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              ManagedOpeningAllocationKey &key) {
    return read_string(bytes, offset, key.operation_key.account.server) &&
           read_u64(bytes, offset, key.operation_key.account.login) &&
           read_u64(bytes, offset, key.operation_key.trade_id) &&
           read_u64(bytes, offset, key.operation_key.operation_id) &&
           read_u64(bytes, offset, key.operation_revision) &&
           read_u64(bytes, offset, key.frontier_revision);
}

void append_reference(std::vector<std::uint8_t> &bytes,
                      const ManagedOpeningAllocationFactReference &reference) {
    append_u64(bytes, reference.deal_ticket);
    append_u64(bytes, reference.source_revision);
    append_u64(bytes, reference.graph_instance_id);
    append_u64(bytes, reference.history_coverage_revision);
    append_u32(bytes, reference.schema_version);
}

bool read_reference(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                    ManagedOpeningAllocationFactReference &reference) {
    return read_u64(bytes, offset, reference.deal_ticket) &&
           read_u64(bytes, offset, reference.source_revision) &&
           read_u64(bytes, offset, reference.graph_instance_id) &&
           read_u64(bytes, offset, reference.history_coverage_revision) &&
           read_u32(bytes, offset, reference.schema_version);
}

std::optional<std::vector<std::uint8_t>> serialize_entry(
    const ManagedOpeningAllocationEntry &entry) {
    if (!entry.valid() || entry.fact_references.size() > kMaxFacts)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_string(body, entry.account.server))
        return std::nullopt;
    append_u64(body, entry.account.login);
    if (!append_key(body, entry.key))
        return std::nullopt;
    append_u64(body, entry.position_identifier);
    append_u64(body, entry.order_ticket);
    append_u32(body, static_cast<std::uint32_t>(entry.direction));
    append_u32(body, entry.quantity_scale);
    append_u64(body, entry.broker_step_units);
    append_u64(body, entry.managed_logical_units_per_step);
    append_u64(body, entry.cumulative_broker_units);
    append_u64(body, entry.cumulative_managed_logical_units);
    append_u64(body, entry.evidence_digest);
    append_u32(body, static_cast<std::uint32_t>(entry.fact_references.size()));
    for (const auto &reference : entry.fact_references)
        append_reference(body, reference);
    if (body.size() > kMaxBodyBytes)
        return std::nullopt;

    std::vector<std::uint8_t> result;
    result.reserve(kEnvelopeBytes + body.size());
    result.insert(result.end(), kMagic.begin(), kMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body.size()));
    append_u64(result, checksum(body));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

ReadStatus read_entry(const std::filesystem::path &path,
                      std::optional<ManagedOpeningAllocationEntry> &entry,
                      std::string &error) {
    entry.reset();
#if defined(_WIN32)
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, kMaxRecordBytes, bytes, error);
    if (status == file_store_support::ReadStatus::absent)
        return ReadStatus::absent;
    if (status == file_store_support::ReadStatus::io_error)
        return ReadStatus::io_error;
    if (bytes.size() < kEnvelopeBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        error = "managed opening allocation has an invalid envelope";
        return ReadStatus::malformed;
    }
    std::size_t offset = kMagic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, offset, version) || !read_u64(bytes, offset, body_size) ||
        !read_u64(bytes, offset, expected_checksum) || version != kFormatVersion ||
        body_size > kMaxBodyBytes || body_size != bytes.size() - offset) {
        error = "managed opening allocation has an unsupported or truncated envelope";
        return ReadStatus::malformed;
    }
    const std::vector<std::uint8_t> body(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    if (checksum(body) != expected_checksum) {
        error = "managed opening allocation checksum mismatch";
        return ReadStatus::malformed;
    }
    std::size_t body_offset = 0;
    ManagedOpeningAllocationEntry value;
    std::uint32_t direction = 0;
    std::uint32_t count = 0;
    if (!read_string(body, body_offset, value.account.server) ||
        !read_u64(body, body_offset, value.account.login) ||
        !read_key(body, body_offset, value.key) ||
        !read_u64(body, body_offset, value.position_identifier) ||
        !read_u64(body, body_offset, value.order_ticket) ||
        !read_u32(body, body_offset, direction) ||
        !read_u32(body, body_offset, value.quantity_scale) ||
        !read_u64(body, body_offset, value.broker_step_units) ||
        !read_u64(body, body_offset, value.managed_logical_units_per_step) ||
        !read_u64(body, body_offset, value.cumulative_broker_units) ||
        !read_u64(body, body_offset, value.cumulative_managed_logical_units) ||
        !read_u64(body, body_offset, value.evidence_digest) ||
        !read_u32(body, body_offset, count) || count > kMaxFacts ||
        count == 0) {
        error = "managed opening allocation body is malformed";
        return ReadStatus::malformed;
    }
    value.direction = static_cast<BrokerPositionDirection>(direction);
    value.fact_references.reserve(count);
    for (std::uint32_t index = 0; index != count; ++index) {
        ManagedOpeningAllocationFactReference reference;
        if (!read_reference(body, body_offset, reference)) {
            error = "managed opening allocation source reference is malformed";
            return ReadStatus::malformed;
        }
        value.fact_references.push_back(reference);
    }
    if (body_offset != body.size() || !value.valid() ||
        value.account != value.key.operation_key.account) {
        error = "managed opening allocation failed structural validation";
        return ReadStatus::malformed;
    }
    entry = std::move(value);
    return ReadStatus::valid;
#else
    (void)path;
    (void)entry;
    error = "WindowsFileManagedOpeningAllocationStore requires Windows";
    return ReadStatus::io_error;
#endif
}

struct DurableOpeningFileRecord {
    ManagedOpeningAllocationEntry entry;
    bool allocation = false;
};

enum class AccountRecordScanStatus { complete, invalid_record, io_error };

struct AccountRecordScanResult {
    AccountRecordScanStatus status = AccountRecordScanStatus::io_error;
    std::vector<DurableOpeningFileRecord> records;
};

AccountRecordScanResult read_account_records_unlocked(
    const std::filesystem::path &directory, const AccountKey &account,
    std::string &error) {
#if defined(_WIN32)
    const auto account_path = account_directory(directory, account);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(account_path, status_error);
    const bool no_such_account =
        status_error == std::make_error_code(std::errc::no_such_file_or_directory) ||
        status_error.value() == ERROR_FILE_NOT_FOUND ||
        status_error.value() == ERROR_PATH_NOT_FOUND;
    if ((status.type() == std::filesystem::file_type::not_found && !status_error) ||
        no_such_account)
        return {AccountRecordScanStatus::complete, {}};
    if (status_error) {
        error = status_error.message();
        return {AccountRecordScanStatus::io_error, {}};
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        error = "managed opening allocation account path is not a directory";
        return {AccountRecordScanStatus::invalid_record, {}};
    }
    const auto account_attributes = GetFileAttributesW(account_path.c_str());
    if (account_attributes == INVALID_FILE_ATTRIBUTES) {
        error = win32_error("GetFileAttributesW(managed opening allocation account)");
        return {AccountRecordScanStatus::io_error, {}};
    }
    if ((account_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "managed opening allocation account path is a reparse point";
        return {AccountRecordScanStatus::invalid_record, {}};
    }

    std::vector<DurableOpeningFileRecord> records;
    try {
        for (const auto &item : std::filesystem::directory_iterator(account_path)) {
            const auto filename = item.path().filename().wstring();
            const bool allocation = filename.rfind(L"alloc-", 0) == 0;
            const bool preparation = filename.rfind(L"prep-", 0) == 0;
            if (!allocation && !preparation)
                continue;
            const auto filename_suffix = filename.size() >= 8
                                             ? filename.substr(filename.size() - 8)
                                             : std::wstring();
            if (filename_suffix == L".bin.tmp") {
                const auto temporary_status = validate_temporary_record(item.path(), error);
                if (temporary_status != TemporaryRecordStatus::valid)
                    return {temporary_status == TemporaryRecordStatus::io_error
                                ? AccountRecordScanStatus::io_error
                                : AccountRecordScanStatus::invalid_record,
                            {}};
                continue;
            }
            if (item.path().extension() != L".bin") {
                error = "managed opening allocation reserved path has an invalid name: " +
                        item.path().filename().string();
                return {AccountRecordScanStatus::invalid_record, {}};
            }
            std::error_code item_status_error;
            const auto item_status = std::filesystem::symlink_status(
                item.path(), item_status_error);
            if (item_status_error) {
                error = item_status_error.message();
                return {AccountRecordScanStatus::io_error, {}};
            }
            if (std::filesystem::is_symlink(item_status) ||
                !std::filesystem::is_regular_file(item_status)) {
                error = "managed opening allocation record is not a regular file";
                return {AccountRecordScanStatus::invalid_record, {}};
            }
            const auto item_attributes = GetFileAttributesW(item.path().c_str());
            if (item_attributes == INVALID_FILE_ATTRIBUTES) {
                error = win32_error("GetFileAttributesW(managed opening allocation record)");
                return {AccountRecordScanStatus::io_error, {}};
            }
            if ((item_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "managed opening allocation record is a reparse point";
                return {AccountRecordScanStatus::invalid_record, {}};
            }
            std::optional<ManagedOpeningAllocationEntry> entry;
            const auto read_status = read_entry(item.path(), entry, error);
            if (read_status != ReadStatus::valid || !entry || entry->account != account) {
                if (error.empty())
                    error = "managed opening allocation record is invalid";
                return {read_status == ReadStatus::io_error
                            ? AccountRecordScanStatus::io_error
                            : AccountRecordScanStatus::invalid_record,
                        {}};
            }
            const auto expected_path = record_path(
                directory, entry->key, allocation ? "alloc-" : "prep-");
            if (expected_path.filename() != item.path().filename()) {
                error = "managed opening allocation record filename is not canonical";
                return {AccountRecordScanStatus::invalid_record, {}};
            }
            records.push_back({std::move(*entry), allocation});
        }
    } catch (const std::filesystem::filesystem_error &filesystem_error) {
        error = filesystem_error.what();
        return {AccountRecordScanStatus::io_error, {}};
    }
    return {AccountRecordScanStatus::complete, std::move(records)};
#else
    (void)directory;
    (void)account;
    error = "WindowsFileManagedOpeningAllocationStore requires Windows";
    return {AccountRecordScanStatus::io_error, {}};
#endif
}

enum class SourceValidation { valid, missing_source, invalid_source, io_error };

enum class ProofRelation { exact, extended, contradiction };

/// \brief Classifies a fresh B3B proof against one immutable historical entry.
/// \param entry Previously committed or prepared frontier.
/// \param proof Fresh verifier result for the same OPEN operation.
/// \return Whether the proof exactly replays, safely extends, or contradicts
///         the historical ticket set and immutable source identity.
ProofRelation classify_proof_relation(
    const ManagedOpeningAllocationEntry &entry,
    const VerifiedManagedOpeningSourceFact &proof) {
    if (!proof.valid() || proof.operation_key() != entry.key.operation_key ||
        proof.operation_revision() != entry.key.operation_revision ||
        proof.position_identifier() != entry.position_identifier)
        return ProofRelation::contradiction;

    for (const auto ticket : entry.fact_references)
        if (!std::binary_search(proof.deal_tickets().begin(), proof.deal_tickets().end(),
                                ticket.deal_ticket))
            return ProofRelation::contradiction;

    const bool exact_ticket_set =
        proof.deal_tickets().size() == entry.fact_references.size();
    if (exact_ticket_set && proof.managed_logical_units() ==
                                entry.cumulative_managed_logical_units &&
        proof.evidence_digest() == entry.evidence_digest)
        return ProofRelation::exact;

    if (!exact_ticket_set &&
        proof.managed_logical_units() > entry.cumulative_managed_logical_units)
        return ProofRelation::extended;
    return ProofRelation::contradiction;
}

ManagedOpeningAllocationCommitStatus map_validation_status(SourceValidation status) {
    if (status == SourceValidation::missing_source)
        return ManagedOpeningAllocationCommitStatus::missing_source;
    if (status == SourceValidation::io_error)
        return ManagedOpeningAllocationCommitStatus::io_error;
    return ManagedOpeningAllocationCommitStatus::invalid_source;
}

ManagedOpeningAllocationCommitStatus map_verifier_commit_status(
    ManagedOpeningSourceVerificationStatus status) {
    switch (status) {
    case ManagedOpeningSourceVerificationStatus::found:
        return ManagedOpeningAllocationCommitStatus::committed;
    case ManagedOpeningSourceVerificationStatus::io_error:
        return ManagedOpeningAllocationCommitStatus::io_error;
    case ManagedOpeningSourceVerificationStatus::unavailable:
        return ManagedOpeningAllocationCommitStatus::missing_source;
    case ManagedOpeningSourceVerificationStatus::invalid_request:
    case ManagedOpeningSourceVerificationStatus::invalid_evidence:
    case ManagedOpeningSourceVerificationStatus::lease_not_held:
        return status == ManagedOpeningSourceVerificationStatus::lease_not_held
                   ? ManagedOpeningAllocationCommitStatus::lease_not_held
                   : ManagedOpeningAllocationCommitStatus::invalid_source;
    }
    return ManagedOpeningAllocationCommitStatus::invalid_source;
}

SourceValidation validate_entry_sources(
    const ManagedOpeningAllocationEntry &entry,
    const VerifiedManagedOpeningSourceFact *proof,
    const DurableJournalStore &journal_store,
    const DurableManagedOpeningFactStore &fact_store,
    std::string &error) {
    if (!entry.valid()) {
        error = "managed opening allocation failed structural validation";
        return SourceValidation::invalid_source;
    }
    if (proof && (!proof->valid() || proof->operation_key() != entry.key.operation_key ||
                  proof->operation_revision() != entry.key.operation_revision ||
                  proof->position_identifier() != entry.position_identifier ||
                  proof->managed_logical_units() != entry.cumulative_managed_logical_units ||
                  proof->evidence_digest() != entry.evidence_digest ||
                  proof->deal_tickets().size() != entry.fact_references.size())) {
        error = "managed opening allocation proof does not match entry";
        return SourceValidation::invalid_source;
    }

    const auto journal = journal_store.load(entry.key.operation_key);
    if (journal.status == StoreLoadStatus::not_found)
        return SourceValidation::missing_source;
    if (journal.status == StoreLoadStatus::io_error)
        return SourceValidation::io_error;
    if (journal.status != StoreLoadStatus::found || !journal.record ||
        !journal.record->valid() || journal.record->operation_kind != OperationKind::open ||
        journal.record->result_source_revision != entry.key.operation_revision ||
        journal.record->journal_state != JournalState::result_persisted ||
        journal.record->result_payload.empty() || !journal.record->reconciliation_descriptor) {
        error = "managed opening allocation journal source is invalid";
        return SourceValidation::invalid_source;
    }

    const auto facts_result = fact_store.scan(entry.account);
    if (facts_result.status == ManagedOpeningFactScanStatus::io_error)
        return SourceValidation::io_error;
    if (facts_result.status != ManagedOpeningFactScanStatus::complete) {
        error = "managed opening allocation fact source is invalid";
        return SourceValidation::invalid_source;
    }
    std::map<std::uint64_t, const ManagedOpeningDealFact *> facts;
    for (const auto &fact : facts_result.facts) {
        if (fact.operation_key() == entry.key.operation_key) {
            if (!fact.valid() || !facts.emplace(fact.deal_ticket(), &fact).second)
                return SourceValidation::invalid_source;
        }
    }
    if (facts.size() < entry.fact_references.size()) {
        error = "managed opening allocation source frontier is incomplete";
        return SourceValidation::invalid_source;
    }
    std::vector<ManagedOpeningDealFact> frontier_facts;
    frontier_facts.reserve(entry.fact_references.size());
    // The key revision is a durable discriminator.  It may be advanced to
    // avoid a collision after an observation-graph restart, so source
    // completeness must use the actual source frontier instead.
    std::map<std::uint64_t, std::uint64_t> source_frontier_revisions;
    for (std::size_t index = 0; index != entry.fact_references.size(); ++index) {
        const auto &reference = entry.fact_references[index];
        const auto fact_it = facts.find(reference.deal_ticket);
        if (fact_it == facts.end()) {
            error = "managed opening allocation references a missing fact";
            return SourceValidation::missing_source;
        }
        if (proof && !std::binary_search(proof->deal_tickets().begin(),
                                         proof->deal_tickets().end(),
                                         reference.deal_ticket)) {
            error = "managed opening allocation proof omits a fact";
            return SourceValidation::invalid_source;
        }
        const auto &fact = *fact_it->second;
        const auto &receipt = fact.admission_receipt();
        if (fact.operation_revision() != entry.key.operation_revision ||
            fact.position_identifier() != entry.position_identifier ||
            fact.order_ticket() != entry.order_ticket ||
            fact.direction() != entry.direction ||
            fact.quantity().canonical_scale() != entry.quantity_scale ||
            fact.quantity().broker_step_units() != entry.broker_step_units ||
            fact.quantity().managed_logical_units() /
                    fact.quantity().integer_step_count() !=
                entry.managed_logical_units_per_step ||
            reference.schema_version != ManagedOpeningDealFact::k_schema_version ||
            reference.deal_ticket != fact.deal_ticket() ||
            reference.source_revision != receipt.source_revision ||
            reference.graph_instance_id != receipt.graph_instance_id ||
            reference.history_coverage_revision != receipt.history_coverage_revision) {
            error = "managed opening allocation fact does not match the entry";
            return SourceValidation::invalid_source;
        }
        auto &graph_frontier_revision =
            source_frontier_revisions[receipt.graph_instance_id];
        graph_frontier_revision =
            (std::max)(graph_frontier_revision, receipt.source_revision);
        frontier_facts.push_back(fact);
    }
    std::uint64_t source_frontier_revision = 0;
    for (const auto &graph_frontier : source_frontier_revisions)
        source_frontier_revision = (std::max)(source_frontier_revision,
                                              graph_frontier.second);
    if (source_frontier_revision > entry.key.frontier_revision) {
        error = "managed opening allocation frontier revision mismatch";
        return SourceValidation::invalid_source;
    }
    if (proof && !std::equal(proof->deal_tickets().begin(), proof->deal_tickets().end(),
                             entry.fact_references.begin(),
                             [](std::uint64_t ticket,
                                const ManagedOpeningAllocationFactReference &reference) {
                                 return ticket == reference.deal_ticket;
                             })) {
        error = "managed opening allocation proof ticket set mismatch";
        return SourceValidation::invalid_source;
    }
    const auto frontier = ManagedOpeningExecutionFrontier::create(std::move(frontier_facts));
    if (!frontier || frontier->cumulative_broker_units() != entry.cumulative_broker_units ||
        frontier->cumulative_managed_logical_units() !=
            entry.cumulative_managed_logical_units) {
        error = "managed opening allocation cumulative quantity mismatch";
        return SourceValidation::invalid_source;
    }
    std::set<std::uint64_t> referenced_tickets;
    for (const auto &reference : entry.fact_references)
        referenced_tickets.insert(reference.deal_ticket);
    for (const auto &fact_pair : facts) {
        const auto &fact = *fact_pair.second;
        if (referenced_tickets.find(fact.deal_ticket()) != referenced_tickets.end())
            continue;
        // A later immutable frontier may add facts. Within one graph epoch an
        // omitted fact from the same or an earlier admitted revision makes
        // this historical snapshot incomplete; cross-instance revisions are
        // not ordered and are bounded by B3B current-history verification.
        const auto &receipt = fact.admission_receipt();
        const auto graph_frontier = source_frontier_revisions.find(
            receipt.graph_instance_id);
        if (!frontier->accepts_identity(fact) ||
            (graph_frontier != source_frontier_revisions.end() &&
             receipt.source_revision <= graph_frontier->second)) {
            error = "managed opening allocation historical frontier is incomplete";
            return SourceValidation::invalid_source;
        }
    }
    return SourceValidation::valid;
}

bool write_entry_file(const std::filesystem::path &target,
                      const ManagedOpeningAllocationEntry &entry,
                      std::string &error) {
#if defined(_WIN32)
    const auto bytes = serialize_entry(entry);
    if (!bytes) {
        error = "managed opening allocation exceeds storage limits";
        return false;
    }
    const auto temporary = std::filesystem::path(target.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, error))
        return false;
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = win32_error("MoveFileExW(managed opening allocation)");
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
#else
    (void)target;
    (void)entry;
    error = "WindowsFileManagedOpeningAllocationStore requires Windows";
    return false;
#endif
}

bool remove_preparation_file(const std::filesystem::path &target, std::string &error) {
#if defined(_WIN32)
    if (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
        return true;
    error = win32_error("DeleteFileW(managed opening preparation)");
    return false;
#else
    (void)target;
    error = "WindowsFileManagedOpeningAllocationStore requires Windows";
    return false;
#endif
}

ManagedOpeningAllocationPublicationStatus map_verifier_status(
    ManagedOpeningSourceVerificationStatus status) {
    switch (status) {
    case ManagedOpeningSourceVerificationStatus::found:
        return ManagedOpeningAllocationPublicationStatus::prepared;
    case ManagedOpeningSourceVerificationStatus::invalid_request:
        return ManagedOpeningAllocationPublicationStatus::invalid_request;
    case ManagedOpeningSourceVerificationStatus::unavailable:
        return ManagedOpeningAllocationPublicationStatus::unavailable;
    case ManagedOpeningSourceVerificationStatus::invalid_evidence:
        return ManagedOpeningAllocationPublicationStatus::invalid_evidence;
    case ManagedOpeningSourceVerificationStatus::io_error:
        return ManagedOpeningAllocationPublicationStatus::io_error;
    case ManagedOpeningSourceVerificationStatus::lease_not_held:
        return ManagedOpeningAllocationPublicationStatus::lease_not_held;
    }
    return ManagedOpeningAllocationPublicationStatus::invalid_evidence;
}

std::optional<ManagedOpeningAllocationEntry> build_entry(
    const VerifiedManagedOpeningSourceFact &proof,
    const ManagedOpeningFactScanResult &facts_result) {
    if (facts_result.status != ManagedOpeningFactScanStatus::complete)
        return std::nullopt;
    std::map<std::uint64_t, const ManagedOpeningDealFact *> by_ticket;
    for (const auto &fact : facts_result.facts)
        if (fact.operation_key() == proof.operation_key() &&
            !by_ticket.emplace(fact.deal_ticket(), &fact).second)
            return std::nullopt;
    std::vector<ManagedOpeningDealFact> facts;
    facts.reserve(proof.deal_tickets().size());
    for (const auto ticket : proof.deal_tickets()) {
        const auto it = by_ticket.find(ticket);
        if (it == by_ticket.end())
            return std::nullopt;
        facts.push_back(*it->second);
    }
    const auto frontier = ManagedOpeningExecutionFrontier::create(facts);
    if (!frontier ||
        frontier->cumulative_managed_logical_units() != proof.managed_logical_units())
        return std::nullopt;
    const auto &basis = frontier->entries().front();
    ManagedOpeningAllocationEntry entry;
    entry.account = proof.operation_key().account;
    entry.key.operation_key = proof.operation_key();
    entry.key.operation_revision = proof.operation_revision();
    entry.position_identifier = proof.position_identifier();
    entry.order_ticket = basis.order_ticket();
    entry.direction = basis.direction();
    entry.quantity_scale = basis.quantity().canonical_scale();
    entry.broker_step_units = basis.quantity().broker_step_units();
    if (basis.quantity().integer_step_count() == 0 ||
        basis.quantity().managed_logical_units() % basis.quantity().integer_step_count() != 0)
        return std::nullopt;
    entry.managed_logical_units_per_step =
        basis.quantity().managed_logical_units() / basis.quantity().integer_step_count();
    entry.cumulative_broker_units = frontier->cumulative_broker_units();
    entry.cumulative_managed_logical_units = frontier->cumulative_managed_logical_units();
    entry.evidence_digest = proof.evidence_digest();
    for (const auto &fact : frontier->entries()) {
        const auto &receipt = fact.admission_receipt();
        entry.key.frontier_revision =
            (std::max)(entry.key.frontier_revision, receipt.source_revision);
        entry.fact_references.push_back({fact.deal_ticket(), receipt.source_revision,
                                         receipt.graph_instance_id,
                                         receipt.history_coverage_revision,
                                         ManagedOpeningDealFact::k_schema_version});
    }
    return entry.valid() ? std::optional<ManagedOpeningAllocationEntry>(std::move(entry))
                         : std::nullopt;
}

bool same_entry_payload(const ManagedOpeningAllocationEntry &left,
                        const ManagedOpeningAllocationEntry &right) {
    if (left.account != right.account ||
        left.key.operation_key != right.key.operation_key ||
        left.key.operation_revision != right.key.operation_revision)
        return false;
    auto normalized_left = left;
    auto normalized_right = right;
    normalized_left.key.frontier_revision = 1;
    normalized_right.key.frontier_revision = 1;
    return normalized_left == normalized_right;
}

enum class SourceEquivalentLookupStatus { not_found, found, invalid_record, io_error };

struct SourceEquivalentLookup {
    SourceEquivalentLookupStatus status = SourceEquivalentLookupStatus::not_found;
    std::optional<ManagedOpeningAllocationEntry> entry;
    bool allocation = false;
};

/// \brief Finds a source-equivalent allocation or preparation under the held account fence.
/// \param directory Allocation namespace, already locked by the caller.
/// \param account Account whose allocation namespace is inspected.
/// \param candidate Source-derived candidate to compare without its discriminator.
/// \param error Receives a fail-closed filesystem or record diagnostic.
/// \return One structurally valid source-equivalent record, or an explicit scan status.
SourceEquivalentLookup find_source_equivalent_unlocked(
    const std::filesystem::path &directory, const AccountKey &account,
    const ManagedOpeningAllocationEntry &candidate, std::string &error) {
    const auto scanned = read_account_records_unlocked(directory, account, error);
    if (scanned.status == AccountRecordScanStatus::io_error)
        return {SourceEquivalentLookupStatus::io_error, std::nullopt};
    if (scanned.status == AccountRecordScanStatus::invalid_record)
        return {SourceEquivalentLookupStatus::invalid_record, std::nullopt};
    std::optional<ManagedOpeningAllocationEntry> allocation_equivalent;
    std::optional<ManagedOpeningAllocationEntry> pending_equivalent;
    for (const auto &record : scanned.records) {
        if (!same_entry_payload(candidate, record.entry))
            continue;
        auto &equivalent = record.allocation ? allocation_equivalent : pending_equivalent;
        if (equivalent) {
            error = "duplicate source-equivalent managed opening records";
            return {SourceEquivalentLookupStatus::invalid_record, std::nullopt};
        }
        equivalent = record.entry;
    }
    if (allocation_equivalent) {
        if (pending_equivalent &&
            !(pending_equivalent->key == allocation_equivalent->key)) {
            error = "source-equivalent allocation and preparation use different keys";
            return {SourceEquivalentLookupStatus::invalid_record, std::nullopt};
        }
        return {SourceEquivalentLookupStatus::found, allocation_equivalent, true};
    }
    if (pending_equivalent)
        return {SourceEquivalentLookupStatus::found, pending_equivalent, false};
    return {};
}

/// \brief Detects two source-equivalent records with different durable keys.
/// \param left First durable record.
/// \param right Second durable record.
/// \return True when both describe one source frontier under different keys.
bool source_identity_conflict(const ManagedOpeningAllocationEntry &left,
                              const ManagedOpeningAllocationEntry &right) {
    return same_entry_payload(left, right) && !(left.key == right.key);
}

/// \brief Selects a durable frontier discriminator without changing source identity.
/// \param candidate Fresh source-derived frontier candidate.
/// \param existing Complete account scan under the allocation store fence.
/// \param error Receives a diagnostic when no discriminator is available.
/// \return Candidate with an idempotent or collision-free durable discriminator.
std::optional<ManagedOpeningAllocationEntry> select_frontier_discriminator(
    const ManagedOpeningAllocationEntry &candidate,
    const ManagedOpeningAllocationScanResult &existing,
    std::string &error) {
    if (!existing.complete()) {
        error = "managed opening allocation namespace could not be scanned";
        return std::nullopt;
    }
    std::vector<const ManagedOpeningAllocationEntry *> entries;
    entries.reserve(existing.records.size() + existing.preparations.size());
    for (const auto &entry : existing.records)
        entries.push_back(&entry);
    for (const auto &preparation : existing.preparations)
        entries.push_back(&preparation.entry);

    const ManagedOpeningAllocationEntry *same_payload = nullptr;
    for (const auto *entry : entries) {
        if (same_entry_payload(candidate, *entry) &&
            (!same_payload ||
             entry->key.frontier_revision < same_payload->key.frontier_revision))
            same_payload = entry;
    }
    if (same_payload) {
        auto selected = candidate;
        selected.key.frontier_revision = same_payload->key.frontier_revision;
        return selected;
    }

    const auto occupied = [&](std::uint64_t frontier_revision) {
        return std::any_of(entries.begin(), entries.end(), [&](const auto *entry) {
            return entry->key.operation_key == candidate.key.operation_key &&
                   entry->key.operation_revision == candidate.key.operation_revision &&
                   entry->key.frontier_revision == frontier_revision;
        });
    };
    if (!occupied(candidate.key.frontier_revision))
        return candidate;

    auto selected = candidate;
    while (selected.key.frontier_revision != (std::numeric_limits<std::uint64_t>::max)()) {
        ++selected.key.frontier_revision;
        if (!occupied(selected.key.frontier_revision))
            return selected;
    }
    error = "managed opening allocation frontier discriminator exhausted";
    return std::nullopt;
}

/// \brief Preserves publication intent before retiring an extended preparation.
/// \param directory Allocation namespace, already locked and account-fenced.
/// \param predecessor Exact durable preparation whose sources must still agree.
/// \param proof Fresh B3B proof for the cumulative successor.
/// \param journal_store Durable journal authority.
/// \param fact_store Durable B3A source facts.
/// \param error Receives a fail-closed diagnostic.
/// \return Committed only after the exact successor is durably represented.
ManagedOpeningAllocationCommitStatus ensure_durable_successor(
    const std::filesystem::path &directory,
    const ManagedOpeningAllocationEntry &predecessor,
    const VerifiedManagedOpeningSourceFact &proof,
    const DurableJournalStore &journal_store,
    const DurableManagedOpeningFactStore &fact_store,
    std::string &error) {
    const auto historical = validate_entry_sources(
        predecessor, nullptr, journal_store, fact_store, error);
    if (historical != SourceValidation::valid)
        return map_validation_status(historical);
    const auto facts = fact_store.scan(predecessor.account);
    if (facts.status == ManagedOpeningFactScanStatus::io_error)
        return ManagedOpeningAllocationCommitStatus::io_error;
    auto successor = build_entry(proof, facts);
    if (!successor || predecessor.key.frontier_revision ==
                          (std::numeric_limits<std::uint64_t>::max)()) {
        error = "managed opening allocation successor cannot be assigned a frontier";
        return ManagedOpeningAllocationCommitStatus::invalid_source;
    }
    successor->key.frontier_revision = (std::max)(
        successor->key.frontier_revision, predecessor.key.frontier_revision + 1);
    const auto equivalent = find_source_equivalent_unlocked(
        directory, predecessor.account, *successor, error);
    if (equivalent.status == SourceEquivalentLookupStatus::io_error)
        return ManagedOpeningAllocationCommitStatus::io_error;
    if (equivalent.status == SourceEquivalentLookupStatus::invalid_record) {
        if (error.empty())
            error = "successor equivalent scan rejected a record";
        return ManagedOpeningAllocationCommitStatus::invalid_record;
    }
    if (equivalent.status == SourceEquivalentLookupStatus::found && equivalent.entry) {
        const auto validation = validate_entry_sources(
            *equivalent.entry, &proof, journal_store, fact_store, error);
        if (validation != SourceValidation::valid)
            return map_validation_status(validation);
        return ManagedOpeningAllocationCommitStatus::committed;
    }
    for (;;) {
        const auto validation = validate_entry_sources(
            *successor, &proof, journal_store, fact_store, error);
        if (validation != SourceValidation::valid) {
            if (error.empty())
                error = "successor source validation failed";
            return map_validation_status(validation);
        }

        bool durable = false;
        bool conflict = false;
        for (const auto *prefix : {"alloc-", "prep-"}) {
            std::optional<ManagedOpeningAllocationEntry> existing;
            const auto target = record_path(directory, successor->key, prefix);
            const auto status = read_entry(target, existing, error);
            if (status == ReadStatus::io_error)
                return ManagedOpeningAllocationCommitStatus::io_error;
            if (status == ReadStatus::malformed)
                return ManagedOpeningAllocationCommitStatus::invalid_record;
            if (status == ReadStatus::valid) {
                if (!existing || !(existing->key == successor->key) ||
                    record_path(directory, existing->key, prefix) != target) {
                    error = "managed opening allocation successor has a non-canonical key";
                    return ManagedOpeningAllocationCommitStatus::invalid_record;
                }
                if (!same_entry_payload(*successor, *existing)) {
                    conflict = true;
                    break;
                }
                durable = true;
            }
        }
        if (conflict) {
            if (successor->key.frontier_revision ==
                (std::numeric_limits<std::uint64_t>::max)())
                break;
            ++successor->key.frontier_revision;
            continue;
        }
        if (!durable && !write_entry_file(
                              record_path(directory, successor->key, "prep-"), *successor,
                              error))
            return ManagedOpeningAllocationCommitStatus::io_error;
        return ManagedOpeningAllocationCommitStatus::committed;
    }
    error = "managed opening allocation successor discriminator exhausted";
    return ManagedOpeningAllocationCommitStatus::invalid_source;
}

ManagedOpeningAllocationPublicationResult map_commit_result(
    ManagedOpeningAllocationCommitStatus status,
    const std::optional<ManagedOpeningAllocationEntry> &entry,
    bool preparation) {
    switch (status) {
    case ManagedOpeningAllocationCommitStatus::committed:
        return {preparation ? ManagedOpeningAllocationPublicationStatus::prepared
                            : ManagedOpeningAllocationPublicationStatus::published,
                entry};
    case ManagedOpeningAllocationCommitStatus::already_committed:
        return {preparation ? ManagedOpeningAllocationPublicationStatus::already_prepared
                            : ManagedOpeningAllocationPublicationStatus::already_published,
                entry};
    case ManagedOpeningAllocationCommitStatus::conflict:
        return {ManagedOpeningAllocationPublicationStatus::conflict, std::nullopt};
    case ManagedOpeningAllocationCommitStatus::lease_not_held:
        return {ManagedOpeningAllocationPublicationStatus::lease_not_held, std::nullopt};
    case ManagedOpeningAllocationCommitStatus::io_error:
        return {ManagedOpeningAllocationPublicationStatus::io_error, std::nullopt};
    case ManagedOpeningAllocationCommitStatus::superseded:
        return {ManagedOpeningAllocationPublicationStatus::superseded, std::nullopt};
    case ManagedOpeningAllocationCommitStatus::missing_source:
    case ManagedOpeningAllocationCommitStatus::invalid_source:
    case ManagedOpeningAllocationCommitStatus::invalid_record:
        return {ManagedOpeningAllocationPublicationStatus::invalid_evidence, std::nullopt};
    }
    return {ManagedOpeningAllocationPublicationStatus::invalid_evidence, std::nullopt};
}

} // namespace

ManagedOpeningAllocationPublicationResult ManagedOpeningAllocationPublisher::prepare(
    const ManagedOpeningProvenanceRequest &request) const {
    if (!request.valid())
        return {ManagedOpeningAllocationPublicationStatus::invalid_request, std::nullopt};
    const auto token = lease_.held_fencing_token(request.operation_key.account);
    if (!token)
        return {ManagedOpeningAllocationPublicationStatus::lease_not_held, std::nullopt};
    ManagedOpeningAllocationPublicationResult result;
    const bool fenced = lease_.with_fencing_capability(
        request.operation_key.account, *token, [&] {
            const auto verified = verifier_.verify(request);
            if (!verified.found()) {
                result.status = map_verifier_status(verified.status);
                return;
            }
            const auto facts = fact_store_.scan(request.operation_key.account);
            const auto entry = build_entry(*verified.proof, facts);
            if (!entry) {
                result.status = ManagedOpeningAllocationPublicationStatus::invalid_evidence;
                return;
            }
            const auto existing = allocation_store_.scan(
                request.operation_key.account, journal_store_, fact_store_);
            if (!existing.complete()) {
                result.status = existing.status == ManagedOpeningAllocationScanStatus::io_error
                                    ? ManagedOpeningAllocationPublicationStatus::io_error
                                    : ManagedOpeningAllocationPublicationStatus::invalid_evidence;
                return;
            }
            std::string selection_error;
            const auto selected = select_frontier_discriminator(
                *entry, existing, selection_error);
            if (!selected) {
                result.status = ManagedOpeningAllocationPublicationStatus::invalid_evidence;
                return;
            }
            result = map_commit_result(
                allocation_store_.prepare(*selected, *verified.proof, journal_store_, fact_store_,
                                           lease_),
                selected, true);
        });
    return fenced ? result
                  : ManagedOpeningAllocationPublicationResult{
                        ManagedOpeningAllocationPublicationStatus::lease_not_held,
                        std::nullopt};
}

ManagedOpeningAllocationPublicationResult ManagedOpeningAllocationPublisher::publish(
    const ManagedOpeningAllocationKey &key,
    const ManagedOpeningProvenanceRequest &request) const {
    if (!request.valid() || !key.valid() || key.operation_key != request.operation_key)
        return {ManagedOpeningAllocationPublicationStatus::invalid_request, std::nullopt};
    auto preparation = allocation_store_.load_preparation(key);
    if (!preparation.found()) {
        if (preparation.status != ManagedOpeningAllocationLoadStatus::not_found)
            return {preparation.status == ManagedOpeningAllocationLoadStatus::io_error
                        ? ManagedOpeningAllocationPublicationStatus::io_error
                        : ManagedOpeningAllocationPublicationStatus::unavailable,
                    std::nullopt};
        const auto committed = allocation_store_.load(key);
        if (!committed.found())
            return {committed.status == ManagedOpeningAllocationLoadStatus::io_error
                        ? ManagedOpeningAllocationPublicationStatus::io_error
                        : ManagedOpeningAllocationPublicationStatus::unavailable,
                    std::nullopt};
        preparation = {ManagedOpeningAllocationLoadStatus::found,
                       ManagedOpeningAllocationPreparation{*committed.record}};
    }
    const auto current = journal_store_.load(request.operation_key);
    if (current.status != StoreLoadStatus::found || !current.record)
        return {current.status == StoreLoadStatus::io_error
                    ? ManagedOpeningAllocationPublicationStatus::io_error
                    : ManagedOpeningAllocationPublicationStatus::unavailable,
                std::nullopt};
    auto current_request = request;
    current_request.operation_revision = current.record->revision;
    const auto status = allocation_store_.publish(
        *preparation.record, journal_store_, fact_store_, lease_, verifier_, current_request);
    return map_commit_result(status, preparation.record->entry, false);
}

WindowsFileManagedOpeningAllocationStore::WindowsFileManagedOpeningAllocationStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            last_error_ = "managed opening allocation directory is empty";
            return;
        }
        const auto absolute = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute);
        if (!std::filesystem::is_directory(absolute)) {
            last_error_ = "managed opening allocation path is not a directory";
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        last_error_ = error.what();
    }
#else
    (void)directory_;
    last_error_ = "WindowsFileManagedOpeningAllocationStore requires Windows";
#endif
}

ManagedOpeningAllocationCommitStatus WindowsFileManagedOpeningAllocationStore::prepare(
    const ManagedOpeningAllocationEntry &entry,
    const VerifiedManagedOpeningSourceFact &proof,
    const DurableJournalStore &journal_store,
    const DurableManagedOpeningFactStore &fact_store,
    const SingleWriterLease &lease) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !entry.valid())
        return ManagedOpeningAllocationCommitStatus::invalid_record;
    const auto token = lease.held_fencing_token(entry.account);
    if (!token)
        return ManagedOpeningAllocationCommitStatus::lease_not_held;
    ManagedOpeningAllocationCommitStatus result =
        ManagedOpeningAllocationCommitStatus::io_error;
    const bool fenced = lease.with_fencing_capability(entry.account, *token, [&] {
        FileLock lock(directory_ / L"managed-opening-allocation.lock");
        if (!lock.acquired()) {
            last_error_ = lock.error();
            return;
        }
        try {
            std::filesystem::create_directories(
                account_directory(directory_, entry.account));
        } catch (const std::filesystem::filesystem_error &error) {
            last_error_ = error.what();
            return;
        }
        const auto equivalent = find_source_equivalent_unlocked(
            directory_, entry.account, entry, last_error_);
        if (equivalent.status == SourceEquivalentLookupStatus::io_error) {
            result = ManagedOpeningAllocationCommitStatus::io_error;
            return;
        }
        if (equivalent.status == SourceEquivalentLookupStatus::invalid_record) {
            result = ManagedOpeningAllocationCommitStatus::invalid_record;
            return;
        }
        if (equivalent.status == SourceEquivalentLookupStatus::found && equivalent.entry) {
            const auto validation = validate_entry_sources(
                *equivalent.entry, nullptr, journal_store, fact_store, last_error_);
            result = validation == SourceValidation::valid
                         ? ManagedOpeningAllocationCommitStatus::already_committed
                         : map_validation_status(validation);
            return;
        }
        const auto validation = validate_entry_sources(
            entry, &proof, journal_store, fact_store, last_error_);
        if (validation != SourceValidation::valid) {
            result = map_validation_status(validation);
            return;
        }
        const auto allocation_target = record_path(directory_, entry.key, "alloc-");
        std::optional<ManagedOpeningAllocationEntry> committed;
        const auto allocation_status = read_entry(
            allocation_target, committed, last_error_);
        if (allocation_status == ReadStatus::malformed) {
            result = ManagedOpeningAllocationCommitStatus::invalid_record;
            return;
        }
        if (allocation_status == ReadStatus::io_error) {
            result = ManagedOpeningAllocationCommitStatus::io_error;
            return;
        }
        const auto target = record_path(directory_, entry.key, "prep-");
        std::optional<ManagedOpeningAllocationEntry> existing;
        const auto read_status = read_entry(target, existing, last_error_);
        if (read_status == ReadStatus::malformed) {
            result = ManagedOpeningAllocationCommitStatus::invalid_record;
            return;
        }
        if (read_status == ReadStatus::io_error) {
            result = ManagedOpeningAllocationCommitStatus::io_error;
            return;
        }
        if (allocation_status == ReadStatus::valid) {
            if (!committed || !(committed->key == entry.key) || !(*committed == entry) ||
                (read_status == ReadStatus::valid && (!existing || !(*existing == entry)))) {
                result = ManagedOpeningAllocationCommitStatus::conflict;
                return;
            }
            result = ManagedOpeningAllocationCommitStatus::already_committed;
            return;
        }
        if (read_status == ReadStatus::valid) {
            result = *existing == entry
                         ? ManagedOpeningAllocationCommitStatus::already_committed
                         : ManagedOpeningAllocationCommitStatus::conflict;
            return;
        }
        result = write_entry_file(target, entry, last_error_)
                     ? ManagedOpeningAllocationCommitStatus::committed
                     : ManagedOpeningAllocationCommitStatus::io_error;
    });
    return fenced ? result : ManagedOpeningAllocationCommitStatus::lease_not_held;
#else
    (void)entry;
    (void)proof;
    (void)journal_store;
    (void)fact_store;
    (void)lease;
    return ManagedOpeningAllocationCommitStatus::io_error;
#endif
}

ManagedOpeningAllocationPreparationLoadResult
WindowsFileManagedOpeningAllocationStore::load_preparation(
    const ManagedOpeningAllocationKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !key.valid())
        return {ManagedOpeningAllocationLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L"managed-opening-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedOpeningAllocationEntry> entry;
    std::string error;
    const auto status = read_entry(record_path(directory_, key, "prep-"), entry, error);
    last_error_ = error;
    if (status == ReadStatus::valid && entry && entry->key == key)
        return {ManagedOpeningAllocationLoadStatus::found,
                ManagedOpeningAllocationPreparation{std::move(*entry)}};
    if (status == ReadStatus::absent)
        return {ManagedOpeningAllocationLoadStatus::not_found, std::nullopt};
    if (status == ReadStatus::io_error)
        return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
    return {ManagedOpeningAllocationLoadStatus::invalid_record, std::nullopt};
#else
    (void)key;
    return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
#endif
}

ManagedOpeningAllocationCommitStatus WindowsFileManagedOpeningAllocationStore::publish(
    const ManagedOpeningAllocationPreparation &preparation,
    const DurableJournalStore &journal_store,
    const DurableManagedOpeningFactStore &fact_store,
    const SingleWriterLease &lease,
    const ManagedOpeningSourceVerifier &verifier,
    const ManagedOpeningProvenanceRequest &request) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !preparation.valid() || !request.valid() ||
        request.operation_key != preparation.entry.key.operation_key)
        return ManagedOpeningAllocationCommitStatus::invalid_record;
    const auto token = lease.held_fencing_token(preparation.entry.account);
    if (!token)
        return ManagedOpeningAllocationCommitStatus::lease_not_held;
    ManagedOpeningAllocationCommitStatus result =
        ManagedOpeningAllocationCommitStatus::io_error;
    const bool fenced = lease.with_fencing_capability(
        preparation.entry.account, *token, [&] {
            FileLock lock(directory_ / L"managed-opening-allocation.lock");
            if (!lock.acquired()) {
                last_error_ = lock.error();
                return;
            }
            std::optional<ManagedOpeningAllocationEntry> durable_preparation;
            std::string durable_error;
            const auto durable_status = read_entry(
                record_path(directory_, preparation.entry.key, "prep-"), durable_preparation,
                durable_error);
            if (durable_status == ReadStatus::malformed ||
                durable_status == ReadStatus::io_error) {
                result = durable_status == ReadStatus::io_error
                             ? ManagedOpeningAllocationCommitStatus::io_error
                             : ManagedOpeningAllocationCommitStatus::invalid_record;
                last_error_ = durable_error;
                return;
            }
            if (durable_status == ReadStatus::valid && durable_preparation &&
                !(*durable_preparation == preparation.entry)) {
                result = ManagedOpeningAllocationCommitStatus::conflict;
                last_error_ = "managed opening allocation preparation conflicts";
                return;
            }
            const bool replay_without_preparation = durable_status == ReadStatus::absent;
            // Publication has one authoritative boundary: reload current
            // sources through the verifier while the account fence is held.
            const auto fresh = verifier.verify(request);
            if (!fresh.found()) {
                result = map_verifier_commit_status(fresh.status);
                last_error_ = "managed opening allocation source verification failed";
                return;
            }
            const auto equivalent = find_source_equivalent_unlocked(
                directory_, preparation.entry.account, preparation.entry, last_error_);
            if (equivalent.status == SourceEquivalentLookupStatus::io_error) {
                result = ManagedOpeningAllocationCommitStatus::io_error;
                return;
            }
            if (equivalent.status == SourceEquivalentLookupStatus::invalid_record) {
                result = ManagedOpeningAllocationCommitStatus::invalid_record;
                return;
            }
            if (equivalent.status == SourceEquivalentLookupStatus::found &&
                equivalent.entry &&
                !(equivalent.entry->key == preparation.entry.key)) {
                if (!equivalent.allocation) {
                    result = ManagedOpeningAllocationCommitStatus::conflict;
                    last_error_ =
                        "managed opening allocation source frontier has a pending alias";
                    return;
                }
                const auto historical_validation = validate_entry_sources(
                    *equivalent.entry, &*fresh.proof, journal_store, fact_store, last_error_);
                if (historical_validation != SourceValidation::valid) {
                    result = map_validation_status(historical_validation);
                    return;
                }
                if (durable_status == ReadStatus::valid &&
                    !remove_preparation_file(
                        record_path(directory_, preparation.entry.key, "prep-"), last_error_)) {
                    result = ManagedOpeningAllocationCommitStatus::io_error;
                    return;
                }
                result = ManagedOpeningAllocationCommitStatus::already_committed;
                return;
            }
            const auto target = record_path(directory_, preparation.entry.key, "alloc-");
            std::optional<ManagedOpeningAllocationEntry> existing;
            const auto read_status = read_entry(target, existing, last_error_);
            if (read_status == ReadStatus::valid) {
                if (!existing || !(*existing == preparation.entry)) {
                    result = ManagedOpeningAllocationCommitStatus::conflict;
                    last_error_ = "managed opening allocation target conflicts with preparation";
                    return;
                }
                const auto historical_validation = validate_entry_sources(
                    *existing, nullptr, journal_store, fact_store, last_error_);
                if (historical_validation != SourceValidation::valid) {
                    result = map_validation_status(historical_validation);
                    return;
                }
                switch (classify_proof_relation(*existing, *fresh.proof)) {
                case ProofRelation::exact:
                case ProofRelation::extended:
                    if (!remove_preparation_file(
                            record_path(directory_, preparation.entry.key, "prep-"),
                            last_error_)) {
                        result = ManagedOpeningAllocationCommitStatus::io_error;
                        return;
                    }
                    result = ManagedOpeningAllocationCommitStatus::already_committed;
                    return;
                case ProofRelation::contradiction:
                    result = ManagedOpeningAllocationCommitStatus::invalid_source;
                    last_error_ = "managed opening allocation source contradicts history";
                    return;
                }
                return;
            }
            if (read_status == ReadStatus::malformed) {
                result = ManagedOpeningAllocationCommitStatus::invalid_record;
                return;
            }
            if (read_status == ReadStatus::io_error) {
                result = ManagedOpeningAllocationCommitStatus::io_error;
                return;
            }
            if (replay_without_preparation) {
                result = ManagedOpeningAllocationCommitStatus::missing_source;
                last_error_ = "managed opening allocation preparation is missing";
                return;
            }
            switch (classify_proof_relation(preparation.entry, *fresh.proof)) {
            case ProofRelation::exact:
                break;
            case ProofRelation::extended: {
                const auto successor_status = ensure_durable_successor(
                    directory_, preparation.entry, *fresh.proof,
                    journal_store, fact_store, last_error_);
                if (successor_status != ManagedOpeningAllocationCommitStatus::committed) {
                    result = successor_status;
                    return;
                }
                if (!remove_preparation_file(
                        record_path(directory_, preparation.entry.key, "prep-"),
                        last_error_)) {
                    result = ManagedOpeningAllocationCommitStatus::io_error;
                    return;
                }
                result = ManagedOpeningAllocationCommitStatus::superseded;
                return;
            }
            case ProofRelation::contradiction:
                result = ManagedOpeningAllocationCommitStatus::invalid_source;
                last_error_ = "managed opening allocation source contradicts preparation";
                return;
            }
            const auto validation = validate_entry_sources(
                preparation.entry, &*fresh.proof, journal_store, fact_store, last_error_);
            if (validation != SourceValidation::valid) {
                result = map_validation_status(validation);
                return;
            }
            result = write_entry_file(target, preparation.entry, last_error_)
                         ? ManagedOpeningAllocationCommitStatus::committed
                         : ManagedOpeningAllocationCommitStatus::io_error;
            if (result == ManagedOpeningAllocationCommitStatus::committed &&
                !remove_preparation_file(
                    record_path(directory_, preparation.entry.key, "prep-"), last_error_))
                result = ManagedOpeningAllocationCommitStatus::io_error;
        });
    return fenced ? result : ManagedOpeningAllocationCommitStatus::lease_not_held;
#else
    (void)preparation;
    (void)journal_store;
    (void)fact_store;
    (void)lease;
    return ManagedOpeningAllocationCommitStatus::io_error;
#endif
}

ManagedOpeningAllocationLoadResult WindowsFileManagedOpeningAllocationStore::load(
    const ManagedOpeningAllocationKey &key) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !key.valid())
        return {ManagedOpeningAllocationLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L"managed-opening-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedOpeningAllocationEntry> entry;
    std::string error;
    const auto status = read_entry(record_path(directory_, key, "alloc-"), entry, error);
    last_error_ = error;
    if (status == ReadStatus::valid && entry && entry->key == key)
        return {ManagedOpeningAllocationLoadStatus::found, std::move(entry)};
    if (status == ReadStatus::absent)
        return {ManagedOpeningAllocationLoadStatus::not_found, std::nullopt};
    if (status == ReadStatus::io_error)
        return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
    return {ManagedOpeningAllocationLoadStatus::invalid_record, std::nullopt};
#else
    (void)key;
    return {ManagedOpeningAllocationLoadStatus::io_error, std::nullopt};
#endif
}

ManagedOpeningAllocationScanResult WindowsFileManagedOpeningAllocationStore::scan(
    const AccountKey &account, const DurableJournalStore &journal_store,
    const DurableManagedOpeningFactStore &fact_store) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_ || !account.valid())
        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
    FileLock lock(directory_ / L"managed-opening-allocation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
    }
    const auto account_path = account_directory(directory_, account);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(account_path, status_error);
    const bool no_such_account =
        status_error == std::make_error_code(std::errc::no_such_file_or_directory) ||
        status_error.value() == ERROR_FILE_NOT_FOUND ||
        status_error.value() == ERROR_PATH_NOT_FOUND;
    if ((status.type() == std::filesystem::file_type::not_found && !status_error) ||
        no_such_account)
        return {ManagedOpeningAllocationScanStatus::complete, {}, {}};
    if (status_error) {
        last_error_ = status_error.message();
        return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
    const auto account_attributes = GetFileAttributesW(account_path.c_str());
    if (account_attributes == INVALID_FILE_ATTRIBUTES) {
        last_error_ = win32_error("GetFileAttributesW(managed opening allocation account)");
        return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
    }
    if ((account_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        last_error_ = "managed opening allocation account path is a reparse point";
        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
    }
    std::vector<ManagedOpeningAllocationEntry> records;
    std::vector<ManagedOpeningAllocationPreparation> preparations;
    std::set<std::string> allocation_identities;
    std::set<std::string> preparation_identities;
    try {
        for (const auto &item : std::filesystem::directory_iterator(account_path)) {
            const auto filename = item.path().filename().wstring();
            const bool allocation = filename.rfind(L"alloc-", 0) == 0;
            const bool preparation = filename.rfind(L"prep-", 0) == 0;
            if (!allocation && !preparation)
                continue;
            const auto filename_suffix = filename.size() >= 8
                                             ? filename.substr(filename.size() - 8)
                                             : std::wstring();
            if (filename_suffix == L".bin.tmp") {
                std::string error;
                const auto temporary_status = validate_temporary_record(item.path(), error);
                if (temporary_status != TemporaryRecordStatus::valid) {
                    last_error_ = error;
                    return {temporary_status == TemporaryRecordStatus::io_error
                                ? ManagedOpeningAllocationScanStatus::io_error
                                : ManagedOpeningAllocationScanStatus::invalid_record,
                            {}, {}};
                }
                continue;
            }
            if (item.path().extension() != L".bin") {
                last_error_ = "managed opening allocation reserved path has an invalid name";
                return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
            }
            std::error_code item_status_error;
            const auto item_status = std::filesystem::symlink_status(item.path(), item_status_error);
            if (item_status_error) {
                last_error_ = item_status_error.message();
                return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
            }
            if (std::filesystem::is_symlink(item_status) ||
                !std::filesystem::is_regular_file(item_status)) {
                last_error_ = "managed opening allocation record is not a regular file";
                return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
            }
            const auto item_attributes = GetFileAttributesW(item.path().c_str());
            if (item_attributes == INVALID_FILE_ATTRIBUTES) {
                last_error_ = win32_error("GetFileAttributesW(managed opening allocation record)");
                return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
            }
            if ((item_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                last_error_ = "managed opening allocation record is a reparse point";
                return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
            }
            std::optional<ManagedOpeningAllocationEntry> entry;
            std::string error;
            const auto read_status = read_entry(item.path(), entry, error);
            if (read_status != ReadStatus::valid || !entry || entry->account != account) {
                last_error_ = error.empty() ? "managed opening allocation record is invalid"
                                            : error;
                return {read_status == ReadStatus::io_error
                            ? ManagedOpeningAllocationScanStatus::io_error
                            : ManagedOpeningAllocationScanStatus::invalid_record,
                        {}, {}};
            }
            const auto expected_path = record_path(
                directory_, entry->key, allocation ? "alloc-" : "prep-");
            if (expected_path.filename() != item.path().filename()) {
                last_error_ = "managed opening allocation record filename is not canonical";
                return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
            }
            if (allocation) {
                if (!allocation_identities.insert(allocation_key_bytes(entry->key)).second) {
                    last_error_ = "duplicate managed opening allocation identity";
                    return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                }
                for (const auto &existing : records)
                    if (source_identity_conflict(*entry, existing)) {
                        last_error_ =
                            "duplicate managed opening source frontier identity";
                        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                    }
                for (const auto &existing : preparations)
                    if (source_identity_conflict(*entry, existing.entry)) {
                        last_error_ =
                            "allocation conflicts with pending source frontier identity";
                        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                    }
                const auto validation = validate_entry_sources(
                    *entry, nullptr, journal_store, fact_store, error);
                if (validation != SourceValidation::valid) {
                    last_error_ = error.empty() ? "managed opening allocation source is invalid"
                                                : error;
                    return {validation == SourceValidation::io_error
                                ? ManagedOpeningAllocationScanStatus::io_error
                                : ManagedOpeningAllocationScanStatus::invalid_record,
                            {}, {}};
                }
                records.push_back(std::move(*entry));
            } else {
                if (!preparation_identities.insert(allocation_key_bytes(entry->key)).second) {
                    last_error_ = "duplicate managed opening preparation identity";
                    return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                }
                for (const auto &existing : records)
                    if (source_identity_conflict(*entry, existing)) {
                        last_error_ =
                            "preparation conflicts with committed source frontier identity";
                        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                    }
                for (const auto &existing : preparations)
                    if (source_identity_conflict(*entry, existing.entry)) {
                        last_error_ =
                            "duplicate pending source frontier identity";
                        return {ManagedOpeningAllocationScanStatus::invalid_record, {}, {}};
                    }
                const auto validation = validate_entry_sources(
                    *entry, nullptr, journal_store, fact_store, error);
                if (validation != SourceValidation::valid) {
                    last_error_ = error.empty() ? "managed opening preparation source is invalid"
                                                : error;
                    return {validation == SourceValidation::io_error
                                ? ManagedOpeningAllocationScanStatus::io_error
                                : ManagedOpeningAllocationScanStatus::invalid_record,
                            {}, {}};
                }
                preparations.push_back({std::move(*entry)});
            }
        }
    } catch (const std::filesystem::filesystem_error &error) {
        last_error_ = error.what();
        return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
    }
    std::sort(records.begin(), records.end(), [](const auto &left, const auto &right) {
        return left.key.frontier_revision < right.key.frontier_revision;
    });
    return {ManagedOpeningAllocationScanStatus::complete, std::move(records),
            std::move(preparations)};
#else
    (void)account;
    (void)journal_store;
    (void)fact_store;
    return {ManagedOpeningAllocationScanStatus::io_error, {}, {}};
#endif
}

} // namespace mt5bridge::dispatch

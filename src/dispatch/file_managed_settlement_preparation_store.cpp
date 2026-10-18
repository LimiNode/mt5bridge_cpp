/// \file file_managed_settlement_preparation_store.cpp
/// \brief Implements staged settlement preparation and restart finalization.

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <mt5bridge/dispatch/file_managed_settlement_preparation_store.hpp>

#include "file_store_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge {
namespace {

constexpr std::array<char, 8> kMagic{{'M', 'T', '5', 'P', 'R', 'P', '0', '1'}};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kMaxEntries = 4096;
constexpr std::size_t kMaxBodyBytes = 2U << 20;
constexpr std::size_t kEnvelopeBytes = kMagic.size() + sizeof(std::uint32_t) +
                                        2U * sizeof(std::uint64_t);
constexpr std::size_t kMaxRecordBytes = kEnvelopeBytes + kMaxBodyBytes;

using namespace file_store_support;

void append_managed_provenance(std::vector<std::uint8_t> &bytes,
                               const ManagedSettlementProvenance &value) {
    append_u64(bytes, value.graph_instance_id);
    append_u64(bytes, value.graph_revision);
    append_u64(bytes, value.history_deals_revision);
    append_u64(bytes, value.evidence_digest);
}

bool read_managed_provenance(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                             ManagedSettlementProvenance &value) {
    return read_u64(bytes, offset, value.graph_instance_id) &&
           read_u64(bytes, offset, value.graph_revision) &&
           read_u64(bytes, offset, value.history_deals_revision) &&
           read_u64(bytes, offset, value.evidence_digest);
}

void append_broker_provenance(std::vector<std::uint8_t> &bytes,
                              const BrokerReversalProvenance &value) {
    append_u64(bytes, value.graph_instance_id);
    append_u64(bytes, value.pre_graph_revision);
    append_u64(bytes, value.post_graph_revision);
    append_u64(bytes, value.pre_positions_revision);
    append_u64(bytes, value.post_positions_revision);
    append_u64(bytes, value.history_deals_revision);
    append_u64(bytes, value.evidence_digest);
}

bool read_broker_provenance(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                            BrokerReversalProvenance &value) {
    return read_u64(bytes, offset, value.graph_instance_id) &&
           read_u64(bytes, offset, value.pre_graph_revision) &&
           read_u64(bytes, offset, value.post_graph_revision) &&
           read_u64(bytes, offset, value.pre_positions_revision) &&
           read_u64(bytes, offset, value.post_positions_revision) &&
           read_u64(bytes, offset, value.history_deals_revision) &&
           read_u64(bytes, offset, value.evidence_digest);
}

bool append_baseline(std::vector<std::uint8_t> &bytes,
                     const ReconciliationBaseline &baseline) {
    if (!baseline.valid() || !append_string(bytes, baseline.account().server))
        return false;
    append_u64(bytes, baseline.account().login);
    append_u64(bytes, baseline.graph_instance_id());
    append_u64(bytes, baseline.graph_revision());
    append_u64(bytes, baseline.active_orders_revision());
    append_u64(bytes, baseline.positions_revision());
    append_u64(bytes, baseline.history_orders_revision());
    append_u64(bytes, baseline.history_deals_revision());
    return true;
}

bool read_baseline(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                   std::optional<ReconciliationBaseline> &baseline) {
    std::string server;
    std::uint64_t login = 0;
    std::uint64_t graph_instance = 0;
    std::uint64_t graph_revision = 0;
    std::uint64_t active_orders = 0;
    std::uint64_t positions = 0;
    std::uint64_t history_orders = 0;
    std::uint64_t history_deals = 0;
    if (!read_string(bytes, offset, server) || !read_u64(bytes, offset, login) ||
        !read_u64(bytes, offset, graph_instance) || !read_u64(bytes, offset, graph_revision) ||
        !read_u64(bytes, offset, active_orders) || !read_u64(bytes, offset, positions) ||
        !read_u64(bytes, offset, history_orders) || !read_u64(bytes, offset, history_deals))
        return false;
    const auto restored = ReconciliationBaseline::restore(
        {std::move(server), login}, graph_instance, graph_revision, active_orders, positions,
        history_orders, history_deals);
    if (!restored)
        return false;
    baseline = *restored;
    return true;
}

bool append_descriptor(std::vector<std::uint8_t> &bytes,
                       const std::optional<ReconciliationDescriptor> &descriptor) {
    append_u32(bytes, descriptor ? 1U : 0U);
    if (!descriptor)
        return true;
    if (!descriptor->valid() || descriptor->predicates.size() > kMaxEntries ||
        descriptor->predicates.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        !append_string(bytes, descriptor->account.server))
        return false;
    append_u64(bytes, descriptor->account.login);
    if (!append_baseline(bytes, descriptor->baseline))
        return false;
    append_u64(bytes, descriptor->trade_id);
    append_u64(bytes, descriptor->operation_id);
    append_u64(bytes, descriptor->requested_volume);
    append_u32(bytes, static_cast<std::uint32_t>(descriptor->settled_state));
    append_u32(bytes, static_cast<std::uint32_t>(descriptor->predicates.size()));
    for (const auto &predicate : descriptor->predicates) {
        append_u32(bytes, static_cast<std::uint32_t>(predicate.kind));
        append_u64(bytes, predicate.ticket);
        append_u32(bytes, predicate.history_window ? 1U : 0U);
        if (predicate.history_window) {
            append_u64(bytes, static_cast<std::uint64_t>(predicate.history_window->from_msc));
            append_u64(bytes, static_cast<std::uint64_t>(predicate.history_window->to_msc));
        }
        append_u32(bytes, predicate.baseline_present
                              ? (*predicate.baseline_present ? 2U : 1U)
                              : 0U);
        append_u64(bytes, predicate.correlation_id);
        append_u32(bytes, static_cast<std::uint32_t>(predicate.expected_transition));
    }
    return true;
}

bool read_descriptor(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                     std::optional<ReconciliationDescriptor> &descriptor) {
    std::uint32_t present = 0;
    if (!read_u32(bytes, offset, present) || present > 1)
        return false;
    if (present == 0)
        return true;
    std::optional<ReconciliationBaseline> baseline;
    std::string account_server;
    std::uint64_t account_login = 0;
    std::uint64_t trade_id = 0;
    std::uint64_t operation_id = 0;
    std::uint64_t requested_volume = 0;
    std::uint32_t settled_state = 0;
    std::uint32_t count = 0;
    if (!read_string(bytes, offset, account_server) || !read_u64(bytes, offset, account_login) ||
        !read_baseline(bytes, offset, baseline) ||
        !read_u64(bytes, offset, trade_id) || !read_u64(bytes, offset, operation_id) ||
        !read_u64(bytes, offset, requested_volume) ||
        !read_u32(bytes, offset, settled_state) || !read_u32(bytes, offset, count) ||
        count > kMaxEntries)
        return false;
    if (!baseline)
        return false;
    ReconciliationDescriptor value{
        {std::move(account_server), account_login}, *baseline, {},
        static_cast<OperationState>(settled_state), trade_id, operation_id,
        requested_volume};
    value.predicates.reserve(count);
    for (std::uint32_t index = 0; index != count; ++index) {
        ReconciliationPredicate predicate;
        std::uint32_t kind = 0;
        std::uint32_t has_window = 0;
        std::uint32_t baseline_present = 0;
        std::uint32_t transition = 0;
        if (!read_u32(bytes, offset, kind) ||
            !read_u64(bytes, offset, predicate.ticket) ||
            !read_u32(bytes, offset, has_window) || has_window > 1)
            return false;
        predicate.kind = static_cast<ReconciliationPredicateKind>(kind);
        if (has_window) {
            std::uint64_t from = 0;
            std::uint64_t to = 0;
            if (!read_u64(bytes, offset, from) || !read_u64(bytes, offset, to))
                return false;
            predicate.history_window =
                ObservationWindow{static_cast<std::int64_t>(from), static_cast<std::int64_t>(to)};
        }
        if (!read_u32(bytes, offset, baseline_present) || baseline_present > 2 ||
            !read_u64(bytes, offset, predicate.correlation_id) ||
            !read_u32(bytes, offset, transition))
            return false;
        if (baseline_present != 0)
            predicate.baseline_present = baseline_present == 2;
        predicate.expected_transition = static_cast<ReconciliationTransition>(transition);
        value.predicates.push_back(std::move(predicate));
    }
    if (!value.valid())
        return false;
    descriptor = std::move(value);
    return true;
}

bool append_operation(std::vector<std::uint8_t> &bytes, const OperationRecord &record) {
    if (!record.valid() || !append_string(bytes, record.key.account.server) ||
        !append_payload(bytes, record.request_payload) ||
        !append_payload(bytes, record.result_payload))
        return false;
    append_u64(bytes, record.key.account.login);
    append_u64(bytes, record.key.trade_id);
    append_u64(bytes, record.key.operation_id);
    append_u32(bytes, static_cast<std::uint32_t>(record.operation_kind));
    append_u32(bytes, static_cast<std::uint32_t>(record.operation_state));
    append_u32(bytes, static_cast<std::uint32_t>(record.journal_state));
    append_u64(bytes, record.revision);
    append_u64(bytes, record.fencing_token);
    append_u64(bytes, record.settled_volume);
    if (!append_descriptor(bytes, record.reconciliation_descriptor) ||
        record.reconciliation_bindings.size() > kMaxEntries ||
        record.reconciliation_bindings.size() >
            (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(record.reconciliation_bindings.size()));
    for (const auto &binding : record.reconciliation_bindings) {
        if (!binding.valid())
            return false;
        append_u64(bytes, binding.correlation_id);
        append_u64(bytes, binding.broker_ticket);
    }
    return true;
}

bool read_operation(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                    OperationRecord &record) {
    if (!read_string(bytes, offset, record.key.account.server) ||
        !read_payload(bytes, offset, record.request_payload) ||
        !read_payload(bytes, offset, record.result_payload) ||
        !read_u64(bytes, offset, record.key.account.login) ||
        !read_u64(bytes, offset, record.key.trade_id) ||
        !read_u64(bytes, offset, record.key.operation_id))
        return false;
    std::uint32_t kind = 0;
    std::uint32_t state = 0;
    std::uint32_t journal = 0;
    std::uint32_t bindings = 0;
    if (!read_u32(bytes, offset, kind) || !read_u32(bytes, offset, state) ||
        !read_u32(bytes, offset, journal) || !read_u64(bytes, offset, record.revision) ||
        !read_u64(bytes, offset, record.fencing_token) ||
        !read_u64(bytes, offset, record.settled_volume) ||
        !read_descriptor(bytes, offset, record.reconciliation_descriptor) ||
        !read_u32(bytes, offset, bindings) || bindings > kMaxEntries)
        return false;
    record.operation_kind = static_cast<OperationKind>(kind);
    record.operation_state = static_cast<OperationState>(state);
    record.journal_state = static_cast<JournalState>(journal);
    record.reconciliation_bindings.reserve(bindings);
    for (std::uint32_t index = 0; index != bindings; ++index) {
        ReconciliationBinding binding;
        if (!read_u64(bytes, offset, binding.correlation_id) ||
            !read_u64(bytes, offset, binding.broker_ticket) || !binding.valid())
            return false;
        record.reconciliation_bindings.push_back(binding);
    }
    return record.valid();
}

bool append_fact(std::vector<std::uint8_t> &bytes, const ManagedDealSettlement &fact) {
    if (!append_string(bytes, fact.key.account.server) ||
        !append_string(bytes, fact.key.operation_key.account.server))
        return false;
    append_u64(bytes, fact.key.account.login);
    append_u64(bytes, fact.key.operation_key.account.login);
    append_u64(bytes, fact.key.operation_key.trade_id);
    append_u64(bytes, fact.key.operation_key.operation_id);
    append_u64(bytes, fact.key.deal_ticket);
    append_u64(bytes, fact.source_operation_revision);
    append_u64(bytes, fact.managed_logical_units);
    append_managed_provenance(bytes, fact.provenance);
    return true;
}

bool read_fact(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
               ManagedDealSettlement &fact) {
    return read_string(bytes, offset, fact.key.account.server) &&
           read_string(bytes, offset, fact.key.operation_key.account.server) &&
           read_u64(bytes, offset, fact.key.account.login) &&
           read_u64(bytes, offset, fact.key.operation_key.account.login) &&
           read_u64(bytes, offset, fact.key.operation_key.trade_id) &&
           read_u64(bytes, offset, fact.key.operation_key.operation_id) &&
           read_u64(bytes, offset, fact.key.deal_ticket) &&
           read_u64(bytes, offset, fact.source_operation_revision) &&
           read_u64(bytes, offset, fact.managed_logical_units) &&
           read_managed_provenance(bytes, offset, fact.provenance) && fact.valid();
}

bool append_frontier(std::vector<std::uint8_t> &bytes,
                     const ManagedSettlementFrontier &frontier) {
    if (frontier.entries.size() > kMaxEntries ||
        frontier.entries.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        !append_string(bytes, frontier.account.server) ||
        !append_string(bytes, frontier.operation_key.account.server))
        return false;
    append_u64(bytes, frontier.account.login);
    append_u64(bytes, frontier.operation_key.account.login);
    append_u64(bytes, frontier.operation_key.trade_id);
    append_u64(bytes, frontier.operation_key.operation_id);
    append_u64(bytes, frontier.operation_revision);
    append_u64(bytes, frontier.settled_volume);
    append_managed_provenance(bytes, frontier.provenance);
    append_u32(bytes, static_cast<std::uint32_t>(frontier.entries.size()));
    for (const auto &entry : frontier.entries) {
        append_u64(bytes, entry.deal_ticket);
        append_u64(bytes, entry.source_operation_revision);
        append_u64(bytes, entry.managed_logical_units);
    }
    return true;
}

bool read_frontier(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                   ManagedSettlementFrontier &frontier) {
    std::uint32_t count = 0;
    if (!read_string(bytes, offset, frontier.account.server) ||
        !read_string(bytes, offset, frontier.operation_key.account.server) ||
        !read_u64(bytes, offset, frontier.account.login) ||
        !read_u64(bytes, offset, frontier.operation_key.account.login) ||
        !read_u64(bytes, offset, frontier.operation_key.trade_id) ||
        !read_u64(bytes, offset, frontier.operation_key.operation_id) ||
        !read_u64(bytes, offset, frontier.operation_revision) ||
        !read_u64(bytes, offset, frontier.settled_volume) ||
        !read_managed_provenance(bytes, offset, frontier.provenance) ||
        !read_u32(bytes, offset, count) || count == 0 || count > kMaxEntries)
        return false;
    frontier.entries.reserve(count);
    for (std::uint32_t index = 0; index != count; ++index) {
        ManagedSettlementFrontierEntry entry;
        if (!read_u64(bytes, offset, entry.deal_ticket) ||
            !read_u64(bytes, offset, entry.source_operation_revision) ||
            !read_u64(bytes, offset, entry.managed_logical_units))
            return false;
        frontier.entries.push_back(entry);
    }
    return frontier.valid();
}

bool append_staged(std::vector<std::uint8_t> &bytes,
                   const ManagedStagedOwnershipAssociation &staged) {
    if (!append_string(bytes, staged.account.server))
        return false;
    append_u64(bytes, staged.account.login);
    if (!append_string(bytes, staged.basis_key.broker_key.account.server))
        return false;
    append_u64(bytes, staged.basis_key.broker_key.account.login);
    append_u64(bytes, staged.basis_key.broker_key.deal_ticket);
    if (!append_string(bytes, staged.basis_key.operation_key.account.server))
        return false;
    append_u64(bytes, staged.basis_key.operation_key.account.login);
    append_u64(bytes, staged.basis_key.operation_key.trade_id);
    append_u64(bytes, staged.basis_key.operation_key.operation_id);
    append_broker_provenance(bytes, staged.broker_provenance);
    append_u64(bytes, staged.base_operation_revision);
    append_u64(bytes, staged.target_operation_revision);
    append_u64(bytes, staged.close_intent_digest);
    return true;
}

bool read_staged(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 ManagedStagedOwnershipAssociation &staged) {
    return read_string(bytes, offset, staged.account.server) &&
           read_u64(bytes, offset, staged.account.login) &&
           read_string(bytes, offset, staged.basis_key.broker_key.account.server) &&
           read_u64(bytes, offset, staged.basis_key.broker_key.account.login) &&
           read_u64(bytes, offset, staged.basis_key.broker_key.deal_ticket) &&
           read_string(bytes, offset, staged.basis_key.operation_key.account.server) &&
           read_u64(bytes, offset, staged.basis_key.operation_key.account.login) &&
           read_u64(bytes, offset, staged.basis_key.operation_key.trade_id) &&
           read_u64(bytes, offset, staged.basis_key.operation_key.operation_id) &&
           read_broker_provenance(bytes, offset, staged.broker_provenance) &&
           read_u64(bytes, offset, staged.base_operation_revision) &&
           read_u64(bytes, offset, staged.target_operation_revision) &&
           read_u64(bytes, offset, staged.close_intent_digest) && staged.valid();
}

std::optional<std::vector<std::uint8_t>> serialize_preparation(
    const ManagedSettlementPreparation &preparation) {
    if (!preparation.valid())
        return std::nullopt;
    std::vector<std::uint8_t> body;
    if (!append_fact(body, preparation.fact) ||
        !append_frontier(body, preparation.frontier))
        return std::nullopt;
    append_u64(body, preparation.base_operation_revision);
    if (!append_staged(body, preparation.staged_ownership) ||
        !append_operation(body, preparation.target_operation) ||
        body.size() > kMaxBodyBytes)
        return std::nullopt;
    std::vector<std::uint8_t> result;
    result.insert(result.end(), kMagic.begin(), kMagic.end());
    append_u32(result, kFormatVersion);
    append_u64(result, static_cast<std::uint64_t>(body.size()));
    append_u64(result, checksum(body));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

std::optional<ManagedSettlementPreparation> deserialize_preparation(
    const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < kEnvelopeBytes || bytes.size() > kMaxRecordBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return std::nullopt;
    std::size_t envelope_offset = kMagic.size();
    std::uint32_t version = 0;
    std::uint64_t body_size = 0;
    std::uint64_t expected_checksum = 0;
    if (!read_u32(bytes, envelope_offset, version) ||
        !read_u64(bytes, envelope_offset, body_size) ||
        !read_u64(bytes, envelope_offset, expected_checksum) || version != kFormatVersion ||
        body_size != bytes.size() - envelope_offset || body_size > kMaxBodyBytes)
        return std::nullopt;
    const std::vector<std::uint8_t> body(
        bytes.begin() + static_cast<std::ptrdiff_t>(envelope_offset), bytes.end());
    if (checksum(body) != expected_checksum)
        return std::nullopt;
    ManagedSettlementPreparation preparation;
    std::size_t offset = 0;
    if (!read_fact(body, offset, preparation.fact)) {
        return std::nullopt;
    }
    if (!read_frontier(body, offset, preparation.frontier)) {
        return std::nullopt;
    }
    if (!read_u64(body, offset, preparation.base_operation_revision)) {
        return std::nullopt;
    }
    if (!read_staged(body, offset, preparation.staged_ownership)) {
        return std::nullopt;
    }
    if (!read_operation(body, offset, preparation.target_operation)) {
        return std::nullopt;
    }
    if (offset != body.size()) {
        return std::nullopt;
    }
    if (!preparation.valid()) {
        return std::nullopt;
    }
    return preparation;
}

#if defined(_WIN32)

ReadStatus read_preparation_file(const std::filesystem::path &path,
                                 std::optional<ManagedSettlementPreparation> &record,
                                 std::string &error) {
    std::vector<std::uint8_t> bytes;
    const auto status = read_bytes_file(path, kMaxRecordBytes, bytes, error,
                                        ReadStatus::malformed);
    if (status != ReadStatus::valid)
        return status;
    record = deserialize_preparation(bytes);
    return record ? ReadStatus::valid : ReadStatus::malformed;
}

#endif

std::filesystem::path preparation_path(const std::filesystem::path &directory,
                                        const ManagedSettlementPreparation &preparation) {
    const auto &key = preparation.frontier.operation_key;
    const auto name = "prepared-" + hex_u64(fnv1a(key.account.server)) + "-" +
                      hex_u64(key.account.login) + "-" + hex_u64(key.trade_id) + "-" +
                      hex_u64(key.operation_id) + "-" +
                      hex_u64(preparation.frontier.operation_revision) + "-" +
                      hex_u64(preparation.fact.key.deal_ticket) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(name);
#else
    return directory / name;
#endif
}

std::filesystem::path preparation_path(const std::filesystem::path &directory,
                                        const OperationKey &operation_key,
                                        std::uint64_t operation_revision,
                                        std::uint64_t deal_ticket) {
    const auto name = "prepared-" + hex_u64(fnv1a(operation_key.account.server)) + "-" +
                      hex_u64(operation_key.account.login) + "-" +
                      hex_u64(operation_key.trade_id) + "-" +
                      hex_u64(operation_key.operation_id) + "-" +
                      hex_u64(operation_revision) + "-" + hex_u64(deal_ticket) + ".bin";
#if defined(_WIN32)
    return directory / widen_ascii(name);
#else
    return directory / name;
#endif
}

void set_error(std::string &target, std::string value) { target = std::move(value); }

ManagedSettlementFinalizeStatus map_settlement_status(
    ManagedDealSettlementCommitStatus status) {
    switch (status) {
    case ManagedDealSettlementCommitStatus::committed:
        return ManagedSettlementFinalizeStatus::finalized;
    case ManagedDealSettlementCommitStatus::already_committed:
        return ManagedSettlementFinalizeStatus::already_finalized;
    case ManagedDealSettlementCommitStatus::missing_operation_record:
        return ManagedSettlementFinalizeStatus::missing_operation_record;
    case ManagedDealSettlementCommitStatus::invalid_operation_record:
        return ManagedSettlementFinalizeStatus::invalid_operation_record;
    case ManagedDealSettlementCommitStatus::missing_fact:
        return ManagedSettlementFinalizeStatus::missing_source;
    case ManagedDealSettlementCommitStatus::io_error:
        return ManagedSettlementFinalizeStatus::io_error;
    default:
        return ManagedSettlementFinalizeStatus::invalid_record;
    }
}

bool frontier_has_allocation_claims(
    const ManagedSettlementFrontier &frontier,
    const std::vector<ManagedAllocationEntry> &allocations,
    std::string &error) {
    for (const auto &frontier_entry : frontier.entries) {
        std::size_t matches = 0;
        for (const auto &allocation : allocations) {
            if (allocation.allocation_key.leg != ManagedAllocationLeg::close ||
                allocation.allocation_key.managed_key.operation_key != frontier.operation_key ||
                allocation.allocation_key.managed_key.deal_ticket !=
                    frontier_entry.deal_ticket ||
                allocation.settlement_source_operation_revision !=
                    frontier_entry.source_operation_revision ||
                allocation.managed_logical_units != frontier_entry.managed_logical_units)
                continue;
            if (!allocation.settlement_frontier_digest ||
                allocation.settlement_frontier_revision > frontier.operation_revision) {
                error = "managed settlement frontier has a legacy or future allocation claim";
                return false;
            }
            ++matches;
        }
        if (matches != 1) {
            error = "managed settlement frontier allocation claim is missing or duplicated";
            return false;
        }
    }
    return true;
}

} // namespace

WindowsFileManagedSettlementPreparationStore::WindowsFileManagedSettlementPreparationStore(
    std::filesystem::path directory)
    : directory_(std::move(directory)) {
#if defined(_WIN32)
    try {
        if (directory_.empty()) {
            set_error(last_error_, "settlement preparation directory is empty");
            return;
        }
        const auto absolute = std::filesystem::absolute(directory_);
        std::filesystem::create_directories(absolute);
        if (!std::filesystem::is_directory(absolute)) {
            set_error(last_error_, "settlement preparation path is not a directory");
            return;
        }
        directory_ = std::filesystem::weakly_canonical(absolute);
        ready_ = true;
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
    }
#else
    set_error(last_error_, "WindowsFileManagedSettlementPreparationStore requires Windows");
#endif
}

ManagedSettlementPreparationStatus
WindowsFileManagedSettlementPreparationStore::prepare(
    const ManagedDealSettlementCommit &proof, const OperationRecord &target_operation,
    std::uint64_t base_operation_revision,
    const ManagedStagedOwnershipAssociation &staged_ownership,
    const DurableJournalStore &journal_store) {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_)
        return ManagedSettlementPreparationStatus::io_error;
    const auto base = journal_store.load(target_operation.key);
    if (base.status == StoreLoadStatus::not_found)
        return ManagedSettlementPreparationStatus::missing_operation_record;
    if (base.status == StoreLoadStatus::io_error)
        return ManagedSettlementPreparationStatus::io_error;
    if (base.status != StoreLoadStatus::found || !base.record ||
        !base.record->valid() || base.record->revision != base_operation_revision)
        return ManagedSettlementPreparationStatus::invalid_operation_record;
    ManagedSettlementPreparation preparation{proof.fact(), proof.frontier(), target_operation,
                                             base_operation_revision, staged_ownership};
    if (!preparation.valid())
        return ManagedSettlementPreparationStatus::invalid_record;

    FileLock lock(directory_ / L".managed-settlement-preparation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return ManagedSettlementPreparationStatus::io_error;
    }
    const auto target = preparation_path(directory_, preparation);
    std::optional<ManagedSettlementPreparation> existing;
    std::string error;
    const auto status = read_preparation_file(target, existing, error);
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return ManagedSettlementPreparationStatus::io_error;
    }
    if (status == ReadStatus::malformed)
        return ManagedSettlementPreparationStatus::invalid_record;
    if (status == ReadStatus::valid && existing)
        return *existing == preparation ? ManagedSettlementPreparationStatus::already_prepared
                                        : ManagedSettlementPreparationStatus::conflict;
    const auto bytes = serialize_preparation(preparation);
    if (!bytes)
        return ManagedSettlementPreparationStatus::invalid_record;
    const std::filesystem::path temporary(target.wstring() + L".tmp");
    DeleteFileW(temporary.c_str());
    if (!write_file(temporary, *bytes, last_error_))
        return ManagedSettlementPreparationStatus::io_error;
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        last_error_ = win32_error("MoveFileExW(settlement preparation)");
        DeleteFileW(temporary.c_str());
        return ManagedSettlementPreparationStatus::io_error;
    }
    return ManagedSettlementPreparationStatus::prepared;
#else
    (void)proof;
    (void)target_operation;
    (void)base_operation_revision;
    (void)staged_ownership;
    (void)journal_store;
    set_error(last_error_, "WindowsFileManagedSettlementPreparationStore requires Windows");
    return ManagedSettlementPreparationStatus::io_error;
#endif
}

ManagedSettlementPreparationLoadResult
WindowsFileManagedSettlementPreparationStore::load(
    const OperationKey &operation_key, std::uint64_t operation_revision,
    std::uint64_t deal_ticket) const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_)
        return {ManagedSettlementPreparationLoadStatus::io_error, std::nullopt};
    if (!operation_key.valid() || operation_revision == 0 || deal_ticket == 0)
        return {ManagedSettlementPreparationLoadStatus::invalid_record, std::nullopt};
    FileLock lock(directory_ / L".managed-settlement-preparation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedSettlementPreparationLoadStatus::io_error, std::nullopt};
    }
    std::optional<ManagedSettlementPreparation> preparation;
    std::string error;
    const auto status = read_preparation_file(
        preparation_path(directory_, operation_key, operation_revision, deal_ticket),
        preparation, error);
    if (status == ReadStatus::absent)
        return {ManagedSettlementPreparationLoadStatus::not_found, std::nullopt};
    if (status == ReadStatus::io_error) {
        set_error(last_error_, error);
        return {ManagedSettlementPreparationLoadStatus::io_error, std::nullopt};
    }
    if (status != ReadStatus::valid || !preparation ||
        preparation->target_operation.key != operation_key ||
        preparation->target_operation.revision != operation_revision ||
        preparation->fact.key.deal_ticket != deal_ticket ||
        preparation_path(directory_, *preparation).filename() !=
            preparation_path(directory_, operation_key, operation_revision, deal_ticket).filename()) {
        set_error(last_error_, error.empty() ? "malformed settlement preparation" : error);
        return {ManagedSettlementPreparationLoadStatus::invalid_record, std::nullopt};
    }
    return {ManagedSettlementPreparationLoadStatus::found, std::move(preparation)};
#else
    (void)operation_key;
    (void)operation_revision;
    (void)deal_ticket;
    set_error(last_error_, "WindowsFileManagedSettlementPreparationStore requires Windows");
    return {ManagedSettlementPreparationLoadStatus::io_error, std::nullopt};
#endif
}

ManagedSettlementFinalizeStatus
WindowsFileManagedSettlementPreparationStore::finalize(
    const ManagedSettlementPreparation &preparation, DurableJournalStore &journal_store,
    DurableManagedDealSettlementStore &settlement_store,
    DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedAllocationStore &allocation_store,
    const SingleWriterLease &lease) {
#if defined(_WIN32)
    if (!preparation.valid())
        return ManagedSettlementFinalizeStatus::invalid_record;
    const auto initial_token = lease.held_fencing_token(preparation.fact.key.account);
    if (!initial_token || *initial_token == 0)
        return ManagedSettlementFinalizeStatus::lease_not_held;
    const auto broker = broker_store.load(preparation.staged_ownership.basis_key.broker_key);
    if (broker.status == BrokerReversalLoadStatus::not_found)
        return ManagedSettlementFinalizeStatus::missing_source;
    if (broker.status == BrokerReversalLoadStatus::io_error)
        return ManagedSettlementFinalizeStatus::io_error;
    if (broker.status != BrokerReversalLoadStatus::found || !broker.record ||
        !broker.record->valid() ||
        !(preparation.staged_ownership.broker_provenance == broker.record->provenance))
        return ManagedSettlementFinalizeStatus::invalid_source;
    if (broker.record->deal_entry == BrokerDealEntry::inout)
        return ManagedSettlementFinalizeStatus::invalid_source;
    const auto mapping = derive_managed_broker_volume_mapping(preparation.fact, *broker.record);
    if (!mapping)
        return ManagedSettlementFinalizeStatus::invalid_source;
    ManagedAllocationEntry expected;
    expected.account = preparation.fact.key.account;
    expected.allocation_key = {preparation.staged_ownership.basis_key.broker_key,
                               preparation.fact.key, ManagedAllocationLeg::close};
    expected.settlement_provenance = preparation.fact.provenance;
    expected.broker_provenance = broker.record->provenance;
    expected.settlement_source_operation_revision =
        preparation.fact.source_operation_revision;
    expected.settlement_frontier_revision = preparation.frontier.operation_revision;
    expected.settlement_frontier_digest =
        managed_settlement_frontier_digest(preparation.frontier);
    expected.basis_operation_revision = preparation.target_operation.revision;
    expected.managed_logical_units = preparation.fact.managed_logical_units;
    expected.broker_volume = mapping->broker_volume();
    if (!expected.valid())
        return ManagedSettlementFinalizeStatus::invalid_record;
    const auto allocation = allocation_store.load(expected.allocation_key);
    if (allocation.status == ManagedAllocationLoadStatus::not_found)
        return ManagedSettlementFinalizeStatus::pending_allocation;
    if (allocation.status == ManagedAllocationLoadStatus::io_error)
        return ManagedSettlementFinalizeStatus::io_error;
    if (allocation.status != ManagedAllocationLoadStatus::found || !allocation.record)
        return ManagedSettlementFinalizeStatus::invalid_source;
    if (!(*allocation.record == expected))
        return ManagedSettlementFinalizeStatus::conflict;

    const auto allocation_scan = allocation_store.scan_recovery(
        settlement_store, broker_store, basis_store, envelope_store, *this);
    if (allocation_scan.status == ManagedAllocationScanStatus::io_error)
        return ManagedSettlementFinalizeStatus::io_error;
    if (!allocation_scan.complete())
        return ManagedSettlementFinalizeStatus::invalid_source;
    std::string allocation_error;
    if (!frontier_has_allocation_claims(
            preparation.frontier, allocation_scan.records, allocation_error))
        return ManagedSettlementFinalizeStatus::invalid_source;

    const auto current = journal_store.load(preparation.target_operation.key);
    if (current.status == StoreLoadStatus::not_found)
        return ManagedSettlementFinalizeStatus::missing_operation_record;
    if (current.status == StoreLoadStatus::io_error)
        return ManagedSettlementFinalizeStatus::io_error;
    if (current.status != StoreLoadStatus::found || !current.record)
        return ManagedSettlementFinalizeStatus::invalid_operation_record;
    if (current.record->revision == preparation.base_operation_revision) {
        StoreCommitStatus commit_status = StoreCommitStatus::io_error;
        const bool capability_held = lease.with_fencing_capability(
            preparation.fact.key.account, *initial_token, [&]() {
                commit_status = journal_store.commit(
                    preparation.target_operation, preparation.base_operation_revision);
            });
        if (!capability_held)
            return ManagedSettlementFinalizeStatus::lease_not_held;
        if (commit_status == StoreCommitStatus::io_error)
            return ManagedSettlementFinalizeStatus::io_error;
        if (commit_status != StoreCommitStatus::committed)
            return ManagedSettlementFinalizeStatus::conflict;
    } else if (current.record->revision != preparation.target_operation.revision ||
               !managed_settlement_preparation_detail::same_operation(
                   *current.record, preparation.target_operation)) {
        return ManagedSettlementFinalizeStatus::conflict;
    }

    const auto current_token = lease.held_fencing_token(preparation.fact.key.account);
    if (!current_token || *current_token != *initial_token)
        return ManagedSettlementFinalizeStatus::lease_not_held;
    ManagedSettlementFinalizeStatus publication_status =
        ManagedSettlementFinalizeStatus::io_error;
    const bool capability_held = lease.with_fencing_capability(
        preparation.fact.key.account, *initial_token, [&]() {
            ManagedDealSettlementCommit proof(preparation.fact, preparation.frontier);
            const auto settlement_status = settlement_store.commit(proof, journal_store);
            const auto mapped = map_settlement_status(settlement_status);
            if (mapped != ManagedSettlementFinalizeStatus::finalized &&
                mapped != ManagedSettlementFinalizeStatus::already_finalized) {
                publication_status = mapped;
                return;
            }
            const auto basis_status = basis_store.commit(
                preparation.staged_ownership.settled_basis(), journal_store, envelope_store);
            if (basis_status == ManagedOwnershipBasisCommitStatus::committed)
                publication_status = ManagedSettlementFinalizeStatus::finalized;
            else if (basis_status == ManagedOwnershipBasisCommitStatus::already_committed)
                publication_status = mapped == ManagedSettlementFinalizeStatus::already_finalized
                                          ? ManagedSettlementFinalizeStatus::already_finalized
                                          : ManagedSettlementFinalizeStatus::finalized;
            else if (basis_status == ManagedOwnershipBasisCommitStatus::io_error)
                publication_status = ManagedSettlementFinalizeStatus::io_error;
            else
                publication_status = ManagedSettlementFinalizeStatus::invalid_source;
        });
    return capability_held ? publication_status
                           : ManagedSettlementFinalizeStatus::lease_not_held;
#else
    (void)preparation;
    (void)journal_store;
    (void)settlement_store;
    (void)basis_store;
    (void)envelope_store;
    (void)broker_store;
    (void)allocation_store;
    (void)lease;
    set_error(last_error_, "WindowsFileManagedSettlementPreparationStore requires Windows");
    return ManagedSettlementFinalizeStatus::io_error;
#endif
}

ManagedSettlementFinalizeStatus
WindowsFileManagedSettlementPreparationStore::recover_pending(
    DurableJournalStore &journal_store, DurableManagedDealSettlementStore &settlement_store,
    DurableManagedOwnershipBasisStore &basis_store,
    const DurableBrokerAllocationStore &envelope_store,
    const DurableBrokerReversalStore &broker_store,
    const DurableManagedAllocationStore &allocation_store,
    const SingleWriterLease &lease) {
    const auto scanned = scan();
    if (!scanned.complete())
        return scanned.status == ManagedSettlementPreparationScanStatus::io_error
                   ? ManagedSettlementFinalizeStatus::io_error
                   : ManagedSettlementFinalizeStatus::invalid_record;
    if (scanned.records.empty())
        return ManagedSettlementFinalizeStatus::already_finalized;
    bool finalized = false;
    bool pending = false;
    for (const auto &preparation : scanned.records) {
        const auto status = finalize(preparation, journal_store, settlement_store, basis_store,
                                     envelope_store, broker_store, allocation_store, lease);
        if (status == ManagedSettlementFinalizeStatus::pending_allocation) {
            pending = true;
            continue;
        }
        if (status != ManagedSettlementFinalizeStatus::finalized &&
            status != ManagedSettlementFinalizeStatus::already_finalized)
            return status;
        finalized = finalized || status == ManagedSettlementFinalizeStatus::finalized;
    }
    return pending ? ManagedSettlementFinalizeStatus::pending_allocation
                   : (finalized ? ManagedSettlementFinalizeStatus::finalized
                                : ManagedSettlementFinalizeStatus::already_finalized);
}

ManagedSettlementPreparationScanResult
WindowsFileManagedSettlementPreparationStore::scan() const {
#if defined(_WIN32)
    last_error_.clear();
    if (!ready_)
        return {ManagedSettlementPreparationScanStatus::io_error, {}};
    FileLock lock(directory_ / L".managed-settlement-preparation.lock");
    if (!lock.acquired()) {
        last_error_ = lock.error();
        return {ManagedSettlementPreparationScanStatus::io_error, {}};
    }
    ManagedSettlementPreparationScanResult result;
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
            const auto filename = entry.path().filename().wstring();
            if (entry.path().extension() != L".bin" || filename.rfind(L"prepared-", 0) != 0)
                continue;
            if (!entry.is_regular_file())
                return {ManagedSettlementPreparationScanStatus::invalid_record, {}};
            std::optional<ManagedSettlementPreparation> preparation;
            std::string error;
            const auto status = read_preparation_file(entry.path(), preparation, error);
            if (status != ReadStatus::valid || !preparation ||
                preparation_path(directory_, *preparation).filename() !=
                    entry.path().filename()) {
                set_error(last_error_, error.empty() ? "malformed settlement preparation" : error);
                return {status == ReadStatus::io_error
                            ? ManagedSettlementPreparationScanStatus::io_error
                            : ManagedSettlementPreparationScanStatus::invalid_record,
                        {}};
            }
            result.records.push_back(std::move(*preparation));
        }
    } catch (const std::filesystem::filesystem_error &error) {
        set_error(last_error_, error.what());
        return {ManagedSettlementPreparationScanStatus::io_error, {}};
    }
    std::sort(result.records.begin(), result.records.end(), [](const auto &left, const auto &right) {
        if (left.frontier.operation_key != right.frontier.operation_key)
            return left.frontier.operation_key < right.frontier.operation_key;
        if (left.frontier.operation_revision != right.frontier.operation_revision)
            return left.frontier.operation_revision < right.frontier.operation_revision;
        return left.fact.key.deal_ticket < right.fact.key.deal_ticket;
    });
    for (std::size_t index = 1; index < result.records.size(); ++index) {
        if (preparation_path(directory_, result.records[index - 1]).filename() ==
            preparation_path(directory_, result.records[index]).filename())
            return {ManagedSettlementPreparationScanStatus::invalid_record, {}};
    }
    result.status = ManagedSettlementPreparationScanStatus::complete;
    return result;
#else
    set_error(last_error_, "WindowsFileManagedSettlementPreparationStore requires Windows");
    return {ManagedSettlementPreparationScanStatus::io_error, {}};
#endif
}

} // namespace mt5bridge

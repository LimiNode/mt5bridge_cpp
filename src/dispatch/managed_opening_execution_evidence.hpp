#pragma once

/// \file dispatch/managed_opening_execution_evidence.hpp
/// \brief Defines the private immutable OPEN execution-evidence contract.

#include "managed_opening_quantity_contract.hpp"

#include <mt5bridge/dispatch/broker_reversal.hpp>
#include <mt5bridge/dispatch/journal_types.hpp>
#include <mt5bridge/trade/deals.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {

/// \struct ManagedOpeningExecutionRequest
/// \brief Exact account, operation, and source constraints for one OPEN lookup.
///
/// These fields constrain a trusted source lookup. They are not proof and are
/// never copied into a sealed evidence value without independent source checks.
struct ManagedOpeningExecutionRequest {
    AccountKey account; ///< Expected terminal account.
    OperationKey operation_key; ///< Expected managed OPEN operation.
    OperationKind operation_kind = OperationKind::open; ///< Expected operation kind.
    std::uint64_t operation_revision = 0; ///< Expected committed journal revision.
    std::uint64_t order_ticket = 0; ///< Expected durable OPEN order ticket.
    std::uint64_t position_identifier = 0; ///< Expected broker position identity.
    std::uint64_t source_revision = 0; ///< Expected history-observation revision.
    std::uint64_t history_coverage_revision = 0; ///< Expected coverage revision.

    /// \brief Tests whether all lookup constraints are usable.
    /// \return True for a complete account-scoped OPEN query.
    bool valid() const {
        return account.valid() && operation_key.valid() &&
               operation_key.account == account &&
               operation_kind == OperationKind::open && operation_revision != 0 &&
               order_ticket != 0 && position_identifier != 0 &&
               source_revision != 0 && history_coverage_revision != 0;
    }
};

/// \struct ManagedOpeningDealObservation
/// \brief Candidate broker/history facts supplied by a trusted observation source.
///
/// This is deliberately not a proof. A future producer must obtain it from the
/// account-scoped observation graph and pass it through the validator before it
/// can construct \c ManagedOpeningDealEvidence.
struct ManagedOpeningDealObservation {
    AccountKey account; ///< Account that admitted the observation.
    OperationKey operation_key; ///< Operation associated with the OPEN order.
    std::uint64_t operation_revision = 0; ///< Journal revision associated with the operation.
    std::uint64_t source_revision = 0; ///< Observation graph revision for the deal.
    std::uint64_t graph_instance_id = 0; ///< Process-local graph provenance identity.
    std::uint64_t history_coverage_revision = 0; ///< Revision proving the history window.
    std::int64_t history_window_from_msc = 0; ///< Inclusive history-window start.
    std::int64_t history_window_to_msc = 0; ///< Inclusive history-window end.
    std::uint64_t order_ticket = 0; ///< DEAL_ORDER and durable OPEN order identity.
    std::uint64_t deal_ticket = 0; ///< DEAL_TICKET identity.
    std::uint64_t position_identifier = 0; ///< DEAL_POSITION_ID identity.
    std::uint64_t known_fields = 0; ///< Source fields present in the observation.
    BrokerPositionDirection direction = BrokerPositionDirection::unknown;
    BrokerDealEntry entry = BrokerDealEntry::unknown;
    std::int64_t deal_time_msc = 0; ///< DEAL_TIME in milliseconds.
    double observed_volume = 0.0; ///< Raw broker volume from the observed deal.
    double observed_step = 0.0; ///< Raw broker symbol step from the same source.
    std::uint64_t managed_logical_units_per_step = 0;
    std::optional<ManagedOpeningQuantity> quantity;
};

/// \brief Deal fields required before an OPEN execution can be attributed.
inline constexpr std::uint64_t kManagedOpeningRequiredDealFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TYPE |
    MT5BRIDGE_DEAL_KNOWN_ENTRY | MT5BRIDGE_DEAL_KNOWN_VOLUME |
    MT5BRIDGE_DEAL_KNOWN_TIME;

/// \enum ManagedOpeningExecutionValidationStatus
/// \brief Explains why an observed OPEN deal cannot become durable evidence.
enum class ManagedOpeningExecutionValidationStatus : std::uint8_t {
    valid = 0,
    invalid_request = 1,
    account_mismatch = 2,
    operation_mismatch = 3,
    order_mismatch = 4,
    position_mismatch = 5,
    source_revision_mismatch = 6,
    missing_history = 7,
    foreign_or_invalid_deal = 8,
    invalid_quantity = 9,
};

/// \brief Validates candidate OPEN evidence against exact source constraints.
/// \param request Caller-independent lookup constraints.
/// \param observation Candidate facts from the durable observation source.
/// \return A fail-closed validation status.
inline ManagedOpeningExecutionValidationStatus validate_managed_opening_observation(
    const ManagedOpeningExecutionRequest &request,
    const ManagedOpeningDealObservation &observation) {
    if (!request.valid())
        return ManagedOpeningExecutionValidationStatus::invalid_request;
    if (!observation.account.valid() || observation.account != request.account)
        return ManagedOpeningExecutionValidationStatus::account_mismatch;
    if (observation.operation_key != request.operation_key ||
        observation.operation_revision != request.operation_revision)
        return ManagedOpeningExecutionValidationStatus::operation_mismatch;
    if (observation.order_ticket != request.order_ticket)
        return ManagedOpeningExecutionValidationStatus::order_mismatch;
    if (observation.position_identifier != request.position_identifier)
        return ManagedOpeningExecutionValidationStatus::position_mismatch;
    if (observation.source_revision != request.source_revision ||
        observation.history_coverage_revision != request.history_coverage_revision ||
        observation.source_revision != observation.history_coverage_revision)
        return ManagedOpeningExecutionValidationStatus::source_revision_mismatch;
    if (observation.graph_instance_id == 0 ||
        observation.history_window_from_msc > observation.history_window_to_msc ||
        observation.deal_time_msc < observation.history_window_from_msc ||
        observation.deal_time_msc > observation.history_window_to_msc)
        return ManagedOpeningExecutionValidationStatus::missing_history;
    if ((observation.known_fields & kManagedOpeningRequiredDealFields) !=
            kManagedOpeningRequiredDealFields ||
        observation.order_ticket == 0 || observation.deal_ticket == 0 ||
        observation.position_identifier == 0 ||
        !valid_broker_position_direction(observation.direction) ||
        observation.entry != BrokerDealEntry::in)
        return ManagedOpeningExecutionValidationStatus::foreign_or_invalid_deal;
    if (!observation.quantity || !observation.quantity->valid() ||
        observation.managed_logical_units_per_step == 0)
        return ManagedOpeningExecutionValidationStatus::invalid_quantity;

    const ManagedOpeningQuantityDomain domain{
        observation.quantity->canonical_scale(),
        observation.quantity->broker_step_units(),
        observation.managed_logical_units_per_step,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
    };
    const auto derived = derive_managed_opening_quantity(
        observation.observed_volume, observation.observed_step, domain);
    if (!derived || derived->canonical_scale() != observation.quantity->canonical_scale() ||
        derived->broker_step_units() != observation.quantity->broker_step_units() ||
        derived->broker_units() != observation.quantity->broker_units() ||
        derived->integer_step_count() != observation.quantity->integer_step_count() ||
        derived->managed_logical_units() != observation.quantity->managed_logical_units())
        return ManagedOpeningExecutionValidationStatus::invalid_quantity;
    return ManagedOpeningExecutionValidationStatus::valid;
}

namespace detail {

/// \class OpeningEvidenceDigest
/// \brief Computes the versioned FNV-1a digest of canonical evidence bytes.
class OpeningEvidenceDigest {
public:
    /// \brief Adds one byte to the digest stream.
    /// \param value Byte to add.
    void add_byte(std::uint8_t value) {
        value_ ^= value;
        value_ *= 1099511628211ULL;
    }

    /// \brief Returns the non-zero digest value.
    /// \return Digest, with zero reserved for invalid evidence.
    std::uint64_t value() const { return value_ == 0 ? 1 : value_; }

private:
    std::uint64_t value_ = 1469598103934665603ULL;
};

inline void append_u64(std::vector<std::uint8_t> *bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes->push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void append_text(std::vector<std::uint8_t> *bytes, const std::string &value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes->insert(bytes->end(), value.begin(), value.end());
}

inline void append_account(std::vector<std::uint8_t> *bytes, const AccountKey &account) {
    append_text(bytes, account.server);
    append_u64(bytes, account.login);
}

inline void append_operation_key(std::vector<std::uint8_t> *bytes,
                                 const OperationKey &key) {
    append_account(bytes, key.account);
    append_u64(bytes, key.trade_id);
    append_u64(bytes, key.operation_id);
}

inline void append_quantity(std::vector<std::uint8_t> *bytes,
                            const ManagedOpeningQuantity &quantity) {
    append_u64(bytes, quantity.canonical_scale());
    append_u64(bytes, quantity.broker_step_units());
    append_u64(bytes, quantity.broker_units());
    append_u64(bytes, quantity.integer_step_count());
    append_u64(bytes, quantity.managed_logical_units());
}

inline std::uint64_t digest_bytes(const std::vector<std::uint8_t> &bytes) {
    OpeningEvidenceDigest digest;
    for (const auto byte : bytes)
        digest.add_byte(byte);
    return digest.value();
}

} // namespace detail

/// \class ManagedOpeningDealEvidence
/// \brief Sealed immutable evidence for one broker OPEN execution deal.
///
/// The constructor is private so caller-supplied identity, quantity, and
/// revision values cannot become authority. A future producer must validate a
/// durable observation source first. The stored digest covers every field
/// except itself through a versioned canonical byte stream.
class ManagedOpeningDealEvidence {
public:
    /// \brief Canonical evidence schema version.
    static constexpr std::uint32_t k_schema_version = 1;

    /// \brief Returns the account identity.
    /// \return Immutable account scope.
    const AccountKey &account() const { return account_; }
    /// \brief Returns the managed operation identity.
    /// \return Immutable operation key.
    const OperationKey &operation_key() const { return operation_key_; }
    /// \brief Returns the committed journal revision.
    /// \return Non-zero operation revision.
    std::uint64_t operation_revision() const { return operation_revision_; }
    /// \brief Returns the broker order ticket.
    /// \return Durable OPEN order identity.
    std::uint64_t order_ticket() const { return order_ticket_; }
    /// \brief Returns the broker deal ticket.
    /// \return Immutable execution identity.
    std::uint64_t deal_ticket() const { return deal_ticket_; }
    /// \brief Returns the broker position identifier.
    /// \return Immutable position identity.
    std::uint64_t position_identifier() const { return position_identifier_; }
    /// \brief Returns the observed deal direction.
    /// \return Buy or sell direction.
    BrokerPositionDirection direction() const { return direction_; }
    /// \brief Returns the observed deal entry mode.
    /// \return Always \c BrokerDealEntry::in for valid evidence.
    BrokerDealEntry entry() const { return entry_; }
    /// \brief Returns the broker execution timestamp.
    /// \return UTC milliseconds.
    std::int64_t deal_time_msc() const { return deal_time_msc_; }
    /// \brief Returns the exact accepted opening quantity.
    /// \return Sealed B2 quantity contract.
    const ManagedOpeningQuantity &quantity() const { return quantity_; }
    /// \brief Returns the observation source revision.
    /// \return Non-zero graph revision.
    std::uint64_t source_revision() const { return source_revision_; }
    /// \brief Returns the graph process identity.
    /// \return Non-zero source graph identity.
    std::uint64_t graph_instance_id() const { return graph_instance_id_; }
    /// \brief Returns the history coverage revision.
    /// \return Revision proving the retained history window.
    std::uint64_t history_coverage_revision() const { return history_coverage_revision_; }
    /// \brief Returns the inclusive history window start.
    /// \return UTC milliseconds.
    std::int64_t history_window_from_msc() const { return history_window_from_msc_; }
    /// \brief Returns the inclusive history window end.
    /// \return UTC milliseconds.
    std::int64_t history_window_to_msc() const { return history_window_to_msc_; }
    /// \brief Returns the canonical evidence digest.
    /// \return Non-zero digest of all immutable fields.
    std::uint64_t evidence_digest() const { return evidence_digest_; }

    /// \brief Returns the canonical versioned byte representation.
    /// \return Deterministically serialized evidence without the digest field.
    std::vector<std::uint8_t> canonical_bytes() const {
        std::vector<std::uint8_t> bytes;
        detail::append_text(&bytes, "mt5bridge-managed-opening-deal-evidence-v1");
        detail::append_u64(&bytes, k_schema_version);
        detail::append_account(&bytes, account_);
        detail::append_operation_key(&bytes, operation_key_);
        detail::append_u64(&bytes, operation_revision_);
        detail::append_u64(&bytes, order_ticket_);
        detail::append_u64(&bytes, deal_ticket_);
        detail::append_u64(&bytes, position_identifier_);
        detail::append_u64(&bytes, static_cast<std::uint64_t>(direction_));
        detail::append_u64(&bytes, static_cast<std::uint64_t>(entry_));
        detail::append_u64(&bytes, static_cast<std::uint64_t>(deal_time_msc_));
        detail::append_quantity(&bytes, quantity_);
        detail::append_u64(&bytes, source_revision_);
        detail::append_u64(&bytes, graph_instance_id_);
        detail::append_u64(&bytes, history_coverage_revision_);
        detail::append_u64(&bytes, static_cast<std::uint64_t>(history_window_from_msc_));
        detail::append_u64(&bytes, static_cast<std::uint64_t>(history_window_to_msc_));
        return bytes;
    }

    /// \brief Tests every immutable evidence invariant and digest.
    /// \return True only for a complete source-backed OPEN deal.
    bool valid() const {
        return account_.valid() && operation_key_.valid() &&
               operation_key_.account == account_ && operation_revision_ != 0 &&
               order_ticket_ != 0 && deal_ticket_ != 0 && position_identifier_ != 0 &&
               valid_broker_position_direction(direction_) &&
               entry_ == BrokerDealEntry::in && deal_time_msc_ >= history_window_from_msc_ &&
               deal_time_msc_ <= history_window_to_msc_ && quantity_.valid() &&
               source_revision_ != 0 && graph_instance_id_ != 0 &&
               history_coverage_revision_ == source_revision_ &&
               history_window_from_msc_ <= history_window_to_msc_ && evidence_digest_ != 0 &&
               evidence_digest_ == detail::digest_bytes(canonical_bytes());
    }

    /// \brief Compares exact immutable evidence fields.
    /// \param other Evidence to compare.
    /// \return True when canonical content is identical.
    bool operator==(const ManagedOpeningDealEvidence &other) const {
        return canonical_bytes() == other.canonical_bytes() &&
               evidence_digest_ == other.evidence_digest_;
    }

    /// \brief Compares exact immutable evidence fields.
    /// \param other Evidence to compare.
    /// \return True when canonical content differs.
    bool operator!=(const ManagedOpeningDealEvidence &other) const {
        return !(*this == other);
    }

private:
    ManagedOpeningDealEvidence(const ManagedOpeningExecutionRequest &request,
                               const ManagedOpeningDealObservation &observation)
        : account_(observation.account), operation_key_(observation.operation_key),
          operation_revision_(observation.operation_revision),
          order_ticket_(observation.order_ticket), deal_ticket_(observation.deal_ticket),
          position_identifier_(observation.position_identifier),
          direction_(observation.direction), entry_(observation.entry),
          deal_time_msc_(observation.deal_time_msc), quantity_(*observation.quantity),
          source_revision_(observation.source_revision),
          graph_instance_id_(observation.graph_instance_id),
          history_coverage_revision_(observation.history_coverage_revision),
          history_window_from_msc_(observation.history_window_from_msc),
          history_window_to_msc_(observation.history_window_to_msc) {
        (void)request;
        evidence_digest_ = detail::digest_bytes(canonical_bytes());
    }

    AccountKey account_;
    OperationKey operation_key_;
    std::uint64_t operation_revision_ = 0;
    std::uint64_t order_ticket_ = 0;
    std::uint64_t deal_ticket_ = 0;
    std::uint64_t position_identifier_ = 0;
    BrokerPositionDirection direction_ = BrokerPositionDirection::unknown;
    BrokerDealEntry entry_ = BrokerDealEntry::unknown;
    std::int64_t deal_time_msc_ = 0;
    ManagedOpeningQuantity quantity_;
    std::uint64_t source_revision_ = 0;
    std::uint64_t graph_instance_id_ = 0;
    std::uint64_t history_coverage_revision_ = 0;
    std::int64_t history_window_from_msc_ = 0;
    std::int64_t history_window_to_msc_ = 0;
    std::uint64_t evidence_digest_ = 0;

    friend class ManagedOpeningExecutionEvidenceProducer;
};

class ManagedOpeningExecutionFrontier;
struct ManagedOpeningExecutionFrontierAppendResult;
ManagedOpeningExecutionFrontierAppendResult append_managed_opening_execution(
    const ManagedOpeningExecutionFrontier &, const ManagedOpeningDealEvidence &);

/// \enum ManagedOpeningExecutionReplayStatus
/// \brief Reports exact replay, conflict, or a new execution identity.
enum class ManagedOpeningExecutionReplayStatus : std::uint8_t {
    new_execution = 0,
    already_committed = 1,
    conflict = 2,
    invalid_evidence = 3,
};

/// \brief Classifies one immutable deal replay against a committed deal.
/// \param existing Existing durable evidence.
/// \param incoming Candidate evidence from a replay.
/// \return Exact replay, same-ticket conflict, new identity, or invalid data.
inline ManagedOpeningExecutionReplayStatus classify_managed_opening_replay(
    const ManagedOpeningDealEvidence &existing,
    const ManagedOpeningDealEvidence &incoming) {
    if (!existing.valid() || !incoming.valid())
        return ManagedOpeningExecutionReplayStatus::invalid_evidence;
    if (existing.deal_ticket() != incoming.deal_ticket())
        return ManagedOpeningExecutionReplayStatus::new_execution;
    return existing == incoming
               ? ManagedOpeningExecutionReplayStatus::already_committed
               : ManagedOpeningExecutionReplayStatus::conflict;
}

/// \class ManagedOpeningExecutionFrontier
/// \brief Immutable cumulative frontier of source-backed OPEN deal evidence.
class ManagedOpeningExecutionFrontier {
public:
    /// \brief Returns the account scope.
    /// \return Immutable account identity.
    const AccountKey &account() const { return account_; }
    /// \brief Returns the operation identity.
    /// \return Immutable OPEN operation key.
    const OperationKey &operation_key() const { return operation_key_; }
    /// \brief Returns the operation journal revision.
    /// \return Non-zero revision.
    std::uint64_t operation_revision() const { return operation_revision_; }
    /// \brief Returns the requested logical opening quantity.
    /// \return Positive bounded target.
    std::uint64_t requested_managed_logical_units() const {
        return requested_managed_logical_units_;
    }
    /// \brief Returns the cumulative logical opening quantity.
    /// \return Sum of immutable per-deal quantities.
    std::uint64_t cumulative_managed_logical_units() const {
        return cumulative_managed_logical_units_;
    }
    /// \brief Returns all immutable deal entries in canonical order.
    /// \return Per-deal source evidence.
    const std::vector<ManagedOpeningDealEvidence> &entries() const { return entries_; }
    /// \brief Tests whether the frontier is terminal.
    /// \return True only when cumulative quantity reaches the requested target.
    bool terminal() const { return terminal_; }
    /// \brief Returns the canonical frontier digest.
    /// \return Non-zero digest of frontier content.
    std::uint64_t frontier_digest() const { return frontier_digest_; }

    /// \brief Returns the canonical versioned frontier bytes.
    /// \return Deterministic serialization with sorted deal entries.
    std::vector<std::uint8_t> canonical_bytes() const {
        std::vector<std::uint8_t> bytes;
        detail::append_text(&bytes, "mt5bridge-managed-opening-frontier-v1");
        detail::append_u64(&bytes, 1);
        detail::append_account(&bytes, account_);
        detail::append_operation_key(&bytes, operation_key_);
        detail::append_u64(&bytes, operation_revision_);
        detail::append_u64(&bytes, requested_managed_logical_units_);
        detail::append_u64(&bytes, cumulative_managed_logical_units_);
        detail::append_u64(&bytes, terminal_ ? 1 : 0);
        detail::append_u64(&bytes, static_cast<std::uint64_t>(entries_.size()));
        for (const auto &entry : entries_) {
            const auto serialized = entry.canonical_bytes();
            detail::append_u64(&bytes, static_cast<std::uint64_t>(serialized.size()));
            bytes.insert(bytes.end(), serialized.begin(), serialized.end());
        }
        return bytes;
    }

    /// \brief Tests all cumulative, identity, and digest invariants.
    /// \return True for a complete partial or terminal frontier.
    bool valid() const {
        if (!account_.valid() || !operation_key_.valid() ||
            operation_key_.account != account_ || operation_revision_ == 0 ||
            requested_managed_logical_units_ == 0 || entries_.empty())
            return false;
        std::uint64_t sum = 0;
        for (std::size_t index = 0; index != entries_.size(); ++index) {
            const auto &entry = entries_[index];
            if (!entry.valid() || entry.account() != account_ ||
                entry.operation_key() != operation_key_ ||
                entry.operation_revision() != operation_revision_ ||
                (index != 0 && entry.deal_ticket() <= entries_[index - 1].deal_ticket()) ||
                sum > (std::numeric_limits<std::uint64_t>::max)() -
                          entry.quantity().managed_logical_units())
                return false;
            for (std::size_t prior = 0; prior < index; ++prior)
                if (entries_[prior].deal_ticket() == entry.deal_ticket())
                    return false;
            sum += entry.quantity().managed_logical_units();
        }
        if (sum != cumulative_managed_logical_units_ || sum == 0 ||
            sum > requested_managed_logical_units_ ||
            overflowed_ ||
            terminal_ != (sum == requested_managed_logical_units_) ||
            frontier_digest_ == 0)
            return false;
        return frontier_digest_ == detail::digest_bytes(canonical_bytes());
    }

    /// \brief Compares exact immutable frontier content.
    /// \param other Frontier to compare.
    /// \return True when canonical bytes and digest match.
    bool operator==(const ManagedOpeningExecutionFrontier &other) const {
        return canonical_bytes() == other.canonical_bytes() &&
               frontier_digest_ == other.frontier_digest_;
    }

private:
    ManagedOpeningExecutionFrontier(AccountKey account, OperationKey operation_key,
                                    std::uint64_t operation_revision,
                                    std::uint64_t requested_managed_logical_units,
                                    std::vector<ManagedOpeningDealEvidence> entries,
                                    bool terminal)
        : account_(std::move(account)), operation_key_(std::move(operation_key)),
          operation_revision_(operation_revision),
          requested_managed_logical_units_(requested_managed_logical_units),
          entries_(std::move(entries)), terminal_(terminal) {
        for (const auto &entry : entries_) {
            const auto units = entry.quantity().managed_logical_units();
            if (units > (std::numeric_limits<std::uint64_t>::max)() -
                           cumulative_managed_logical_units_) {
                cumulative_managed_logical_units_ =
                    (std::numeric_limits<std::uint64_t>::max)();
                overflowed_ = true;
                break;
            }
            cumulative_managed_logical_units_ += units;
        }
        frontier_digest_ = detail::digest_bytes(canonical_bytes());
    }

    AccountKey account_;
    OperationKey operation_key_;
    std::uint64_t operation_revision_ = 0;
    std::uint64_t requested_managed_logical_units_ = 0;
    std::uint64_t cumulative_managed_logical_units_ = 0;
    std::vector<ManagedOpeningDealEvidence> entries_;
    bool terminal_ = false;
    bool overflowed_ = false;
    std::uint64_t frontier_digest_ = 0;

    friend class ManagedOpeningExecutionEvidenceProducer;
    friend struct ManagedOpeningExecutionFrontierAppendResult;
    friend ManagedOpeningExecutionFrontierAppendResult append_managed_opening_execution(
        const ManagedOpeningExecutionFrontier &, const ManagedOpeningDealEvidence &);
};

/// \enum ManagedOpeningExecutionFrontierAppendStatus
/// \brief Reports one immutable frontier extension attempt.
enum class ManagedOpeningExecutionFrontierAppendStatus : std::uint8_t {
    appended = 0,
    duplicate = 1,
    conflict = 2,
    invalid_frontier = 3,
    invalid_evidence = 4,
    operation_mismatch = 5,
    source_revision_mismatch = 6,
    terminal_frontier = 7,
};

/// \struct ManagedOpeningExecutionFrontierAppendResult
/// \brief Carries an immutable frontier extension or a fail-closed status.
struct ManagedOpeningExecutionFrontierAppendResult {
    ManagedOpeningExecutionFrontierAppendStatus status =
        ManagedOpeningExecutionFrontierAppendStatus::invalid_frontier;
    std::optional<ManagedOpeningExecutionFrontier> frontier;

    /// \brief Tests whether a new frontier was published.
    /// \return True only for the appended status and a valid frontier.
    bool appended() const {
        return status == ManagedOpeningExecutionFrontierAppendStatus::appended &&
               frontier && frontier->valid();
    }
};

/// \brief Appends one later source-backed OPEN deal without mutating history.
/// \param frontier Existing immutable partial or terminal frontier.
/// \param evidence New deal evidence from a strictly later source revision.
/// \return New frontier, duplicate/conflict status, or fail-closed status.
inline ManagedOpeningExecutionFrontierAppendResult append_managed_opening_execution(
    const ManagedOpeningExecutionFrontier &frontier,
    const ManagedOpeningDealEvidence &evidence) {
    if (!frontier.valid())
        return {ManagedOpeningExecutionFrontierAppendStatus::invalid_frontier, std::nullopt};
    if (!evidence.valid())
        return {ManagedOpeningExecutionFrontierAppendStatus::invalid_evidence, std::nullopt};
    for (const auto &existing : frontier.entries()) {
        if (existing.deal_ticket() != evidence.deal_ticket())
            continue;
        return {existing == evidence
                    ? ManagedOpeningExecutionFrontierAppendStatus::duplicate
                    : ManagedOpeningExecutionFrontierAppendStatus::conflict,
                std::nullopt};
    }
    if (frontier.operation_key() != evidence.operation_key() ||
        frontier.operation_revision() != evidence.operation_revision())
        return {ManagedOpeningExecutionFrontierAppendStatus::operation_mismatch,
                std::nullopt};
    if (frontier.terminal())
        return {ManagedOpeningExecutionFrontierAppendStatus::terminal_frontier, std::nullopt};
    std::uint64_t latest_source_revision = 0;
    for (const auto &existing : frontier.entries())
        latest_source_revision =
            latest_source_revision > existing.source_revision()
                ? latest_source_revision
                : existing.source_revision();
    if (evidence.source_revision() <= latest_source_revision)
        return {ManagedOpeningExecutionFrontierAppendStatus::source_revision_mismatch,
                std::nullopt};
    const auto added = evidence.quantity().managed_logical_units();
    if (added > frontier.requested_managed_logical_units() -
                    frontier.cumulative_managed_logical_units())
        return {ManagedOpeningExecutionFrontierAppendStatus::conflict, std::nullopt};
    auto entries = frontier.entries();
    entries.push_back(evidence);
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return left.deal_ticket() < right.deal_ticket();
    });
    const bool terminal = frontier.cumulative_managed_logical_units() + added ==
                           frontier.requested_managed_logical_units();
    ManagedOpeningExecutionFrontier next(
        frontier.account(), frontier.operation_key(), frontier.operation_revision(),
        frontier.requested_managed_logical_units(), std::move(entries), terminal);
    return {ManagedOpeningExecutionFrontierAppendStatus::appended, std::move(next)};
}

} // namespace mt5bridge::dispatch

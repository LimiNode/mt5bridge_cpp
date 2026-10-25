#pragma once

/// \file dispatch/managed_close_attribution_contract.hpp
/// \brief Defines the private C3 managed-position attribution contract.

#include "managed_inout_close_delta_candidate.hpp"

#include <mt5bridge/dispatch/managed_broker_volume_mapping.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {

/// \enum ManagedPositionOwnerKind
/// \brief Classifies one complete pre-position ownership entry.
enum class ManagedPositionOwnerKind : std::uint8_t {
    managed_trade = 1, ///< Volume is backed by one managed trade's open allocation.
    unmanaged = 2, ///< Volume is explicitly known but has no managed owner.
};

/// \struct ManagedPositionOwnershipFrontierKey
/// \brief Identifies the exact pre-deal position observation behind a frontier.
struct ManagedPositionOwnershipFrontierKey {
    AccountKey account; ///< Account scope of the observed netting position.
    std::uint64_t position_identifier = 0; ///< Stable broker position identifier.
    std::uint64_t graph_instance_id = 0; ///< Observation graph instance identity.
    std::uint64_t graph_revision = 0; ///< Exact graph revision containing the position.
    std::uint64_t positions_revision = 0; ///< Exact position-domain revision.

    /// \brief Tests whether the exact observation identity is usable.
    /// \return True for a complete, ordered identity.
    bool valid() const {
        return account.valid() && position_identifier != 0 && graph_instance_id != 0 &&
               graph_revision != 0 && positions_revision != 0 &&
               positions_revision <= graph_revision;
    }

    /// \brief Compares complete frontier identities.
    /// \param other Identity to compare.
    /// \return True when every field matches.
    bool operator==(const ManagedPositionOwnershipFrontierKey &other) const {
        return account == other.account &&
               position_identifier == other.position_identifier &&
               graph_instance_id == other.graph_instance_id &&
               graph_revision == other.graph_revision &&
               positions_revision == other.positions_revision;
    }
};

/// \struct ManagedPositionOwnershipEntry
/// \brief Assigns one exact part of a broker position to managed or unmanaged origin.
struct ManagedPositionOwnershipEntry {
    ManagedPositionOwnerKind owner_kind = ManagedPositionOwnerKind::unmanaged;
    std::uint64_t trade_id = 0; ///< Managed owner, or zero for unmanaged volume.
    std::uint64_t managed_logical_units = 0; ///< Managed units, or zero if unmanaged.
    BrokerVolume broker_volume; ///< Exact broker-position quantity represented.
    std::optional<OperationKey> source_operation; ///< Managed OPEN source when applicable.
    std::uint64_t source_operation_revision = 0; ///< Exact source frontier revision.
    std::uint64_t source_evidence_digest = 0; ///< Immutable source-allocation identity.

    /// \brief Tests local ownership and exact-volume invariants.
    /// \param account Expected account scope.
    /// \return True for one complete managed or explicitly unmanaged entry.
    bool valid(const AccountKey &account) const {
        if (!account.valid() || !broker_volume.valid() || source_evidence_digest == 0)
            return false;
        if (owner_kind == ManagedPositionOwnerKind::managed_trade) {
            if (trade_id == 0 || managed_logical_units == 0 || !source_operation ||
                !source_operation->valid() || source_operation->account != account ||
                source_operation->trade_id != trade_id ||
                source_operation_revision == 0)
                return false;
            const auto expected = map_managed_logical_units_to_broker_volume(
                managed_logical_units,
                BrokerVolumeNormalization{broker_volume.scale,
                                          broker_volume.step_units});
            return expected && *expected == broker_volume;
        }
        return owner_kind == ManagedPositionOwnerKind::unmanaged && trade_id == 0 &&
               managed_logical_units == 0 && !source_operation &&
               source_operation_revision == 0;
    }

    /// \brief Compares exact ownership-entry fields.
    /// \param other Entry to compare.
    /// \return True when every field matches.
    bool operator==(const ManagedPositionOwnershipEntry &other) const {
        return owner_kind == other.owner_kind && trade_id == other.trade_id &&
               managed_logical_units == other.managed_logical_units &&
               broker_volume == other.broker_volume &&
               source_operation == other.source_operation &&
               source_operation_revision == other.source_operation_revision &&
               source_evidence_digest == other.source_evidence_digest;
    }
};

/// \struct ManagedPositionOwnershipFrontier
/// \brief Complete ownership decomposition of one observed broker pre-position.
///
/// A future production store may publish this record only from validated,
/// durable opening allocations and their committed consumption history. The C3
/// prerequisite provides no such producer.
struct ManagedPositionOwnershipFrontier {
    ManagedPositionOwnershipFrontierKey key; ///< Exact pre-position observation.
    std::string symbol; ///< Canonical broker symbol.
    BrokerPositionDirection direction = BrokerPositionDirection::unknown;
    BrokerVolume position_volume; ///< Complete observed pre-position volume.
    std::uint64_t source_evidence_digest = 0; ///< Complete source-set identity.
    std::vector<ManagedPositionOwnershipEntry> entries; ///< Full ownership partition.

    /// \brief Tests completeness, exact normalization, and duplicate sources.
    /// \return True only when entries partition the complete position volume.
    bool valid() const {
        if (!key.valid() || symbol.empty() || symbol.size() >= 64 ||
            !valid_broker_position_direction(direction) || !position_volume.valid() ||
            source_evidence_digest == 0 || entries.empty())
            return false;
        std::uint64_t total_broker_units = 0;
        for (std::size_t index = 0; index != entries.size(); ++index) {
            const auto &entry = entries[index];
            if (!entry.valid(key.account) ||
                entry.broker_volume.scale != position_volume.scale ||
                entry.broker_volume.step_units != position_volume.step_units ||
                total_broker_units > (std::numeric_limits<std::uint64_t>::max)() -
                                         entry.broker_volume.units)
                return false;
            for (std::size_t prior = 0; prior != index; ++prior) {
                if (entries[prior].owner_kind == entry.owner_kind &&
                    entries[prior].source_operation == entry.source_operation &&
                    entries[prior].source_operation_revision ==
                        entry.source_operation_revision &&
                    entries[prior].source_evidence_digest ==
                        entry.source_evidence_digest)
                    return false;
            }
            total_broker_units += entry.broker_volume.units;
        }
        return total_broker_units == position_volume.units;
    }

    /// \brief Compares complete ownership-frontier fields.
    /// \param other Frontier to compare.
    /// \return True when every field and entry matches.
    bool operator==(const ManagedPositionOwnershipFrontier &other) const {
        return key == other.key && symbol == other.symbol &&
               direction == other.direction && position_volume == other.position_volume &&
               source_evidence_digest == other.source_evidence_digest &&
               entries == other.entries;
    }
};

namespace detail {

inline bool managed_position_entry_less(const ManagedPositionOwnershipEntry &left,
                                        const ManagedPositionOwnershipEntry &right) {
    const auto left_kind = static_cast<std::uint8_t>(left.owner_kind);
    const auto right_kind = static_cast<std::uint8_t>(right.owner_kind);
    if (left_kind != right_kind)
        return left_kind < right_kind;
    if (left.trade_id != right.trade_id)
        return left.trade_id < right.trade_id;
    if (left.source_operation.has_value() != right.source_operation.has_value())
        return left.source_operation.has_value() < right.source_operation.has_value();
    if (left.source_operation && right.source_operation) {
        if (left.source_operation->account.server !=
            right.source_operation->account.server)
            return left.source_operation->account.server <
                   right.source_operation->account.server;
        if (left.source_operation->account.login !=
            right.source_operation->account.login)
            return left.source_operation->account.login <
                   right.source_operation->account.login;
        if (left.source_operation->trade_id != right.source_operation->trade_id)
            return left.source_operation->trade_id < right.source_operation->trade_id;
        if (left.source_operation->operation_id !=
            right.source_operation->operation_id)
            return left.source_operation->operation_id <
                   right.source_operation->operation_id;
    }
    if (left.source_operation_revision != right.source_operation_revision)
        return left.source_operation_revision < right.source_operation_revision;
    if (left.source_evidence_digest != right.source_evidence_digest)
        return left.source_evidence_digest < right.source_evidence_digest;
    if (left.managed_logical_units != right.managed_logical_units)
        return left.managed_logical_units < right.managed_logical_units;
    if (left.broker_volume.units != right.broker_volume.units)
        return left.broker_volume.units < right.broker_volume.units;
    if (left.broker_volume.scale != right.broker_volume.scale)
        return left.broker_volume.scale < right.broker_volume.scale;
    return left.broker_volume.step_units < right.broker_volume.step_units;
}

inline void managed_position_digest_u64(std::uint64_t *digest, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
        *digest ^= static_cast<std::uint8_t>(value >> shift);
        *digest *= 1099511628211ULL;
    }
}

inline void managed_position_digest_text(std::uint64_t *digest,
                                         const std::string &value) {
    managed_position_digest_u64(digest, static_cast<std::uint64_t>(value.size()));
    for (const unsigned char byte : value)
        managed_position_digest_u64(digest, byte);
}

} // namespace detail

/// \brief Computes a canonical deterministic identity for one ownership frontier.
/// \param frontier Complete ownership frontier.
/// \return Non-zero digest, or zero when the frontier is invalid.
/// \note The digest detects structural drift; it is not an anti-forgery primitive.
inline std::uint64_t managed_position_ownership_frontier_digest(
    const ManagedPositionOwnershipFrontier &frontier) {
    if (!frontier.valid())
        return 0;
    auto entries = frontier.entries;
    std::sort(entries.begin(), entries.end(), detail::managed_position_entry_less);
    std::uint64_t digest = 1469598103934665603ULL;
    detail::managed_position_digest_text(
        &digest, "mt5bridge-managed-position-ownership-frontier-v1");
    detail::managed_position_digest_text(&digest, frontier.key.account.server);
    detail::managed_position_digest_u64(&digest, frontier.key.account.login);
    detail::managed_position_digest_u64(&digest,
                                        frontier.key.position_identifier);
    detail::managed_position_digest_u64(&digest, frontier.key.graph_instance_id);
    detail::managed_position_digest_u64(&digest, frontier.key.graph_revision);
    detail::managed_position_digest_u64(&digest, frontier.key.positions_revision);
    detail::managed_position_digest_text(&digest, frontier.symbol);
    detail::managed_position_digest_u64(
        &digest, static_cast<std::uint64_t>(frontier.direction));
    detail::managed_position_digest_u64(&digest, frontier.position_volume.units);
    detail::managed_position_digest_u64(&digest, frontier.position_volume.scale);
    detail::managed_position_digest_u64(&digest,
                                        frontier.position_volume.step_units);
    detail::managed_position_digest_u64(&digest, frontier.source_evidence_digest);
    detail::managed_position_digest_u64(&digest,
                                        static_cast<std::uint64_t>(entries.size()));
    for (const auto &entry : entries) {
        detail::managed_position_digest_u64(
            &digest, static_cast<std::uint64_t>(entry.owner_kind));
        detail::managed_position_digest_u64(&digest, entry.trade_id);
        detail::managed_position_digest_u64(&digest,
                                            entry.managed_logical_units);
        detail::managed_position_digest_u64(&digest, entry.broker_volume.units);
        detail::managed_position_digest_u64(&digest, entry.broker_volume.scale);
        detail::managed_position_digest_u64(&digest,
                                            entry.broker_volume.step_units);
        detail::managed_position_digest_u64(
            &digest, entry.source_operation.has_value() ? 1 : 0);
        if (entry.source_operation) {
            detail::managed_position_digest_text(
                &digest, entry.source_operation->account.server);
            detail::managed_position_digest_u64(
                &digest, entry.source_operation->account.login);
            detail::managed_position_digest_u64(
                &digest, entry.source_operation->trade_id);
            detail::managed_position_digest_u64(
                &digest, entry.source_operation->operation_id);
        }
        detail::managed_position_digest_u64(
            &digest, entry.source_operation_revision);
        detail::managed_position_digest_u64(
            &digest, entry.source_evidence_digest);
    }
    return digest == 0 ? 1 : digest;
}

/// \enum ManagedPositionOwnershipLoadStatus
/// \brief Reports one durable ownership-frontier lookup outcome.
enum class ManagedPositionOwnershipLoadStatus : std::uint8_t {
    found = 0,
    not_found = 1,
    invalid_record = 2,
    io_error = 3,
};

/// \struct ManagedPositionOwnershipLoadResult
/// \brief Carries one exact durable ownership-frontier lookup.
struct ManagedPositionOwnershipLoadResult {
    ManagedPositionOwnershipLoadStatus status =
        ManagedPositionOwnershipLoadStatus::not_found;
    std::optional<ManagedPositionOwnershipFrontier> record;

    /// \brief Tests whether a record was returned.
    /// \return True only for a found record.
    bool found() const {
        return status == ManagedPositionOwnershipLoadStatus::found &&
               record.has_value();
    }
};

/// \class DurableManagedPositionOwnershipSource
/// \brief Contract for exact pre-deal managed-position ownership evidence.
///
/// No production implementation exists in this slice. A future implementation
/// must derive records from committed opening allocations and subsequent
/// committed consumption, then preserve them across restart; callers cannot
/// manufacture a frontier from the C2 candidate.
class DurableManagedPositionOwnershipSource {
public:
    virtual ~DurableManagedPositionOwnershipSource() = default;

    /// \brief Loads the exact pre-position ownership frontier.
    /// \param key Exact account, position, graph, and domain revision identity.
    /// \return Durable lookup outcome.
    virtual ManagedPositionOwnershipLoadResult load(
        const ManagedPositionOwnershipFrontierKey &key) const = 0;
};

/// \enum ManagedInoutCloseAttributionStatus
/// \brief Classifies the fail-closed C3 prerequisite assessment.
enum class ManagedInoutCloseAttributionStatus : std::uint8_t {
    authorized = 0,
    invalid_candidate = 1,
    invalid_broker = 2,
    missing_frontier = 3,
    invalid_frontier = 4,
    stale_frontier = 5,
    ambiguous_managed_ownership = 6,
    unmanaged_position_residue = 7,
    unsupported_partial_close = 8,
    io_error = 9,
};

/// \class ManagedInoutCloseAttributionAuthorization
/// \brief Sealed result of matching a candidate to exclusive pre-position ownership.
///
/// This private value is a contract fixture only. No allocation store or owner
/// path accepts it until a production durable ownership source is implemented.
class ManagedInoutCloseAttributionAuthorization {
public:
    /// \brief Returns the authorized close operation.
    /// \return Managed operation identity.
    const OperationKey &operation_key() const { return operation_key_; }

    /// \brief Returns the exact broker deal.
    /// \return DEAL_TICKET.
    std::uint64_t deal_ticket() const { return deal_ticket_; }

    /// \brief Returns the complete managed logical delta.
    /// \return Exact exclusive-owner units.
    std::uint64_t managed_logical_delta() const { return managed_logical_delta_; }

    /// \brief Returns the exact consumed broker close leg.
    /// \return Step-normalized broker volume.
    const BrokerVolume &broker_close_leg() const { return broker_close_leg_; }

    /// \brief Returns the exact source-frontier identity.
    /// \return Pre-deal ownership lookup key.
    const ManagedPositionOwnershipFrontierKey &frontier_key() const {
        return frontier_key_;
    }

    /// \brief Returns the canonical source-frontier digest.
    /// \return Non-zero deterministic frontier identity.
    std::uint64_t frontier_digest() const { return frontier_digest_; }

    /// \brief Returns the structural candidate identity.
    /// \return Non-zero candidate digest.
    std::uint64_t candidate_digest() const { return candidate_digest_; }

    /// \brief Tests the sealed authorization's local identity fields.
    /// \return True for a complete exact binding.
    bool valid() const {
        return operation_key_.valid() && deal_ticket_ != 0 &&
               managed_logical_delta_ != 0 && broker_close_leg_.valid() &&
               frontier_key_.valid() && frontier_key_.account == operation_key_.account &&
               frontier_digest_ != 0 && candidate_digest_ != 0 &&
               broker_provenance_.valid();
    }

private:
    ManagedInoutCloseAttributionAuthorization(
        OperationKey operation_key, std::uint64_t deal_ticket,
        std::uint64_t managed_logical_delta, BrokerVolume broker_close_leg,
        ManagedPositionOwnershipFrontierKey frontier_key,
        std::uint64_t frontier_digest, std::uint64_t candidate_digest,
        BrokerReversalProvenance broker_provenance)
        : operation_key_(std::move(operation_key)), deal_ticket_(deal_ticket),
          managed_logical_delta_(managed_logical_delta),
          broker_close_leg_(broker_close_leg), frontier_key_(std::move(frontier_key)),
          frontier_digest_(frontier_digest), candidate_digest_(candidate_digest),
          broker_provenance_(broker_provenance) {}

    OperationKey operation_key_;
    std::uint64_t deal_ticket_ = 0;
    std::uint64_t managed_logical_delta_ = 0;
    BrokerVolume broker_close_leg_;
    ManagedPositionOwnershipFrontierKey frontier_key_;
    std::uint64_t frontier_digest_ = 0;
    std::uint64_t candidate_digest_ = 0;
    BrokerReversalProvenance broker_provenance_;

    friend class ManagedInoutCloseAttributionAuthorizer;
};

/// \struct ManagedInoutCloseAttributionResult
/// \brief Carries one fail-closed authority assessment.
struct ManagedInoutCloseAttributionResult {
    ManagedInoutCloseAttributionStatus status =
        ManagedInoutCloseAttributionStatus::invalid_candidate;
    std::optional<ManagedInoutCloseAttributionAuthorization> authorization;

    /// \brief Tests whether exclusive ownership was established.
    /// \return True only for an authorized result with a valid sealed value.
    bool authorized() const {
        return status == ManagedInoutCloseAttributionStatus::authorized &&
               authorization && authorization->valid();
    }
};

/// \class ManagedInoutCloseAttributionAuthorizer
/// \brief Validates the narrow exact-exclusive C3 attribution contract.
class ManagedInoutCloseAttributionAuthorizer {
public:
    /// \brief Assesses a structural candidate against independent durable ownership.
    /// \param candidate C2 structural candidate for the observed INOUT deal.
    /// \param reversal Durable broker reversal proof.
    /// \param source Independent exact pre-position ownership source.
    /// \return Authorized only for a complete full close owned exclusively by
    ///         the candidate's managed trade.
    static ManagedInoutCloseAttributionResult assess(
        const ManagedInoutCloseDeltaCandidate &candidate,
        const BrokerReversalRecord &reversal,
        const DurableManagedPositionOwnershipSource &source) {
        if (!candidate.valid())
            return {ManagedInoutCloseAttributionStatus::invalid_candidate,
                    std::nullopt};
        if (!reversal.valid() || candidate.operation_key().account != reversal.account ||
            candidate.deal_ticket() != reversal.deal_ticket ||
            !(candidate.ownership_key().broker_key == reversal.key()) ||
            !(candidate.broker_provenance() == reversal.provenance))
            return {ManagedInoutCloseAttributionStatus::invalid_broker,
                    std::nullopt};

        const ManagedPositionOwnershipFrontierKey frontier_key{
            reversal.account, reversal.pre_position_identifier,
            reversal.provenance.graph_instance_id,
            reversal.provenance.pre_graph_revision,
            reversal.provenance.pre_positions_revision};
        const auto loaded = source.load(frontier_key);
        if (loaded.status == ManagedPositionOwnershipLoadStatus::io_error)
            return {ManagedInoutCloseAttributionStatus::io_error, std::nullopt};
        if (loaded.status == ManagedPositionOwnershipLoadStatus::not_found)
            return {ManagedInoutCloseAttributionStatus::missing_frontier,
                    std::nullopt};
        if (loaded.status != ManagedPositionOwnershipLoadStatus::found ||
            !loaded.record || !loaded.record->valid())
            return {ManagedInoutCloseAttributionStatus::invalid_frontier,
                    std::nullopt};

        const auto &frontier = *loaded.record;
        if (!(frontier.key == frontier_key) || frontier.symbol != reversal.symbol ||
            frontier.direction != reversal.pre_direction ||
            !(frontier.position_volume == reversal.pre_volume) ||
            !(frontier.position_volume == reversal.broker_close_leg))
            return {ManagedInoutCloseAttributionStatus::stale_frontier,
                    std::nullopt};

        std::uint64_t managed_total = 0;
        for (const auto &entry : frontier.entries) {
            if (entry.owner_kind == ManagedPositionOwnerKind::unmanaged)
                return {ManagedInoutCloseAttributionStatus::unmanaged_position_residue,
                        std::nullopt};
            if (entry.trade_id != candidate.operation_key().trade_id)
                return {
                    ManagedInoutCloseAttributionStatus::ambiguous_managed_ownership,
                    std::nullopt};
            if (!entry.source_operation ||
                entry.source_operation->operation_id >=
                    candidate.operation_key().operation_id ||
                managed_total > (std::numeric_limits<std::uint64_t>::max)() -
                                    entry.managed_logical_units)
                return {ManagedInoutCloseAttributionStatus::invalid_frontier,
                        std::nullopt};
            managed_total += entry.managed_logical_units;
        }

        const auto &pre = candidate.pre_exposure();
        const auto &post = candidate.post_exposure();
        if (managed_total == 0 || pre.open_volume != managed_total ||
            candidate.managed_close_intent_volume() != managed_total ||
            candidate.managed_logical_delta() != managed_total ||
            post.open_volume != 0 || !post.close_satisfied ||
            post.slice_state != managed_trade::OperationState::filled)
            return {ManagedInoutCloseAttributionStatus::unsupported_partial_close,
                    std::nullopt};

        const auto mapped = map_managed_logical_units_to_broker_volume(
            managed_total,
            BrokerVolumeNormalization{reversal.broker_close_leg.scale,
                                      reversal.broker_close_leg.step_units});
        if (!mapped || !(*mapped == reversal.broker_close_leg))
            return {ManagedInoutCloseAttributionStatus::invalid_frontier,
                    std::nullopt};
        const auto frontier_digest =
            managed_position_ownership_frontier_digest(frontier);
        if (frontier_digest == 0)
            return {ManagedInoutCloseAttributionStatus::invalid_frontier,
                    std::nullopt};

        ManagedInoutCloseAttributionAuthorization authorization(
            candidate.operation_key(), candidate.deal_ticket(), managed_total,
            reversal.broker_close_leg, frontier_key, frontier_digest,
            candidate.evidence_digest(), reversal.provenance);
        if (!authorization.valid())
            return {ManagedInoutCloseAttributionStatus::invalid_frontier,
                    std::nullopt};
        return {ManagedInoutCloseAttributionStatus::authorized,
                std::move(authorization)};
    }
};

} // namespace mt5bridge::dispatch

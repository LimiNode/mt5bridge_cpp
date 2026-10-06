#pragma once

/// \file dispatch/broker_reversal.hpp
/// \brief Defines the durable broker-level INOUT reversal contract.

#include <mt5bridge/reconciliation/graph.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mt5bridge {

/// \enum BrokerMarginMode
/// \brief Account margin modes understood by the broker reversal proof.
enum class BrokerMarginMode : std::uint32_t {
    retail_netting = 0, ///< MT5 retail netting account.
    exchange = 1,       ///< MT5 exchange/netting account.
    retail_hedging = 2, ///< MT5 retail hedging account.
};

/// \brief Tests whether an account margin mode is a known MT5 mode.
/// \param mode Candidate account margin mode.
/// \return True when the mode is one of the supported MT5 values.
constexpr bool valid_broker_margin_mode(BrokerMarginMode mode) {
    return mode == BrokerMarginMode::retail_netting ||
           mode == BrokerMarginMode::exchange ||
           mode == BrokerMarginMode::retail_hedging;
}

/// \brief Tests whether an account mode has one netting position namespace.
/// \param mode Candidate account margin mode.
/// \return True for retail-netting and exchange accounts.
constexpr bool broker_margin_mode_is_netting(BrokerMarginMode mode) {
    return mode == BrokerMarginMode::retail_netting ||
           mode == BrokerMarginMode::exchange;
}

/// \enum BrokerPositionDirection
/// \brief Direction of one observed broker position or deal.
enum class BrokerPositionDirection : std::uint32_t {
    unknown = 0,
    buy = 1,
    sell = 2,
};

/// \brief Tests whether a position direction is usable.
/// \param direction Candidate direction.
/// \return True for buy or sell.
constexpr bool valid_broker_position_direction(BrokerPositionDirection direction) {
    return direction == BrokerPositionDirection::buy ||
           direction == BrokerPositionDirection::sell;
}

/// \brief Tests whether two directions are opposite.
/// \param left First direction.
/// \param right Second direction.
/// \return True only for a buy/sell pair.
constexpr bool opposite_broker_position_directions(
    BrokerPositionDirection left, BrokerPositionDirection right) {
    return (left == BrokerPositionDirection::buy &&
            right == BrokerPositionDirection::sell) ||
           (left == BrokerPositionDirection::sell &&
            right == BrokerPositionDirection::buy);
}

/// \enum BrokerPositionIdentityRelation
/// \brief Describes whether a reversal retained or replaced its position identity.
enum class BrokerPositionIdentityRelation : std::uint32_t {
    continuous = 1, ///< Pre and post observations carry one identifier.
    replaced = 2,   ///< Reversal produced a new position identifier.
};

/// \enum BrokerDealPositionBinding
/// \brief Identifies which proven position identity the deal references.
enum class BrokerDealPositionBinding : std::uint32_t {
    pre = 1,  ///< DEAL_POSITION_ID matches the pre-deal position.
    post = 2, ///< DEAL_POSITION_ID matches the post-deal position.
};

/// \struct BrokerVolume
/// \brief Exact broker volume expressed in canonical symbol-step units.
///
/// `scale` is the decimal scale selected by the symbol-volume proof and
/// `step_units` is the configured symbol step at that scale. The record never
/// compares source MT5 doubles directly; all persisted volume arithmetic is
/// performed on `units` at one common scale.
struct BrokerVolume {
    std::uint64_t units = 0; ///< Positive volume units.
    std::uint32_t scale = 0; ///< Decimal denominator exponent, at most nine.
    std::uint64_t step_units = 0; ///< Positive configured symbol-step units.

    /// \brief Tests whether the volume is a positive representable quantity.
    /// \return True when units are non-zero, step-aligned, and scale is bounded.
    bool valid() const {
        return units != 0 && scale <= 9 && step_units != 0 &&
               units % step_units == 0;
    }

    /// \brief Compares two exact volumes.
    /// \param other Volume to compare.
    /// \return True when units and scale match exactly.
    bool operator==(const BrokerVolume &other) const {
        return units == other.units && scale == other.scale &&
               step_units == other.step_units;
    }
};

/// \struct BrokerReversalProvenance
/// \brief Immutable before/after observation proof for one broker reversal.
struct BrokerReversalProvenance {
    std::uint64_t graph_instance_id = 0; ///< Historical graph instance identity.
    std::uint64_t pre_graph_revision = 0; ///< Revision containing pre evidence.
    std::uint64_t post_graph_revision = 0; ///< Later revision containing post evidence.
    std::uint64_t pre_positions_revision = 0; ///< Position-domain pre revision.
    std::uint64_t post_positions_revision = 0; ///< Position-domain post revision.
    std::uint64_t history_deals_revision = 0; ///< Revision covering the deal.
    std::uint64_t evidence_digest = 0; ///< Stable digest of exact source evidence.

    /// \brief Tests whether the proof has an ordered, complete provenance chain.
    /// \return True when all revisions and the evidence digest are usable.
    bool valid() const {
        return graph_instance_id != 0 && pre_graph_revision != 0 &&
               post_graph_revision > pre_graph_revision &&
               pre_positions_revision != 0 &&
               post_positions_revision > pre_positions_revision &&
               history_deals_revision != 0 && evidence_digest != 0;
    }

    /// \brief Compares two provenance proofs.
    /// \param other Proof to compare.
    /// \return True when every provenance component matches.
    bool operator==(const BrokerReversalProvenance &other) const {
        return graph_instance_id == other.graph_instance_id &&
               pre_graph_revision == other.pre_graph_revision &&
               post_graph_revision == other.post_graph_revision &&
               pre_positions_revision == other.pre_positions_revision &&
               post_positions_revision == other.post_positions_revision &&
               history_deals_revision == other.history_deals_revision &&
               evidence_digest == other.evidence_digest;
    }
};

/// \struct BrokerReversalKey
/// \brief Account-scoped immutable identity of one broker deal.
struct BrokerReversalKey {
    AccountKey account; ///< Terminal account identity.
    std::uint64_t deal_ticket = 0; ///< MT5 DEAL_TICKET.

    /// \brief Tests whether the key can address one durable record.
    /// \return True when account and deal ticket are usable.
    bool valid() const { return account.valid() && deal_ticket != 0; }

    /// \brief Compares two broker reversal keys.
    /// \param other Key to compare.
    /// \return True when account and deal ticket match.
    bool operator==(const BrokerReversalKey &other) const {
        return account == other.account && deal_ticket == other.deal_ticket;
    }

    /// \brief Orders keys for deterministic scans.
    /// \param other Key to compare.
    /// \return True when this key sorts before `other`.
    bool operator<(const BrokerReversalKey &other) const {
        if (account.server != other.account.server)
            return account.server < other.account.server;
        if (account.login != other.account.login)
            return account.login < other.account.login;
        return deal_ticket < other.deal_ticket;
    }
};

/// \struct BrokerReversalObservation
/// \brief Fresh broker evidence from which one decomposition is derived.
///
/// The caller supplies exact, step-normalized volumes. This keeps conversion
/// from MT5 floating-point fields at the observation boundary and prevents the
/// durable record from performing approximate arithmetic.
struct BrokerReversalObservation {
    AccountKey account; ///< Terminal account identity.
    BrokerMarginMode margin_mode = BrokerMarginMode::retail_netting;
    std::string symbol; ///< Canonical symbol shared by deal and positions.
    std::uint64_t deal_ticket = 0; ///< MT5 DEAL_TICKET.
    std::uint64_t order_ticket = 0; ///< MT5 DEAL_ORDER.
    std::uint64_t deal_position_id = 0; ///< MT5 DEAL_POSITION_ID.
    BrokerDealPositionBinding deal_position_binding =
        BrokerDealPositionBinding::post;
    std::uint64_t pre_position_identifier = 0;
    BrokerPositionDirection pre_direction = BrokerPositionDirection::unknown;
    BrokerVolume pre_volume;
    std::uint64_t post_position_identifier = 0;
    BrokerPositionDirection post_direction = BrokerPositionDirection::unknown;
    BrokerVolume post_volume;
    BrokerPositionDirection deal_direction = BrokerPositionDirection::unknown;
    BrokerPositionIdentityRelation identity_relation =
        BrokerPositionIdentityRelation::continuous;
    BrokerVolume deal_volume;
    BrokerReversalProvenance provenance;
};

/// \struct BrokerReversalRecord
/// \brief Durable broker-level decomposition of one INOUT deal.
///
/// This record deliberately contains no managed `TradeId`, allocation, or
/// exposure mutation. Its validity proves only that one netting-account deal
/// consumed the complete pre-position and created the complete post-position.
struct BrokerReversalRecord {
    AccountKey account; ///< Terminal account identity.
    BrokerMarginMode margin_mode = BrokerMarginMode::retail_netting;
    std::string symbol; ///< Canonical symbol from all three observations.
    std::uint64_t deal_ticket = 0; ///< MT5 DEAL_TICKET.
    std::uint64_t order_ticket = 0; ///< MT5 DEAL_ORDER.
    std::uint64_t deal_position_id = 0; ///< MT5 DEAL_POSITION_ID.
    BrokerDealPositionBinding deal_position_binding =
        BrokerDealPositionBinding::post;
    std::uint64_t pre_position_identifier = 0; ///< POSITION_IDENTIFIER before deal.
    BrokerPositionDirection pre_direction = BrokerPositionDirection::unknown;
    BrokerVolume pre_volume;
    std::uint64_t post_position_identifier = 0; ///< POSITION_IDENTIFIER after deal.
    BrokerPositionDirection post_direction = BrokerPositionDirection::unknown;
    BrokerVolume post_volume;
    BrokerPositionDirection deal_direction = BrokerPositionDirection::unknown;
    BrokerPositionIdentityRelation identity_relation =
        BrokerPositionIdentityRelation::continuous;
    BrokerVolume deal_volume;
    BrokerVolume broker_close_leg;
    BrokerVolume broker_reverse_open_leg;
    BrokerReversalProvenance provenance;

    /// \brief Returns the immutable deal identity.
    /// \return Account and deal-ticket key.
    BrokerReversalKey key() const { return {account, deal_ticket}; }

    /// \brief Tests every broker decomposition and provenance invariant.
    /// \return True only for a complete, netting-account reversal proof.
    bool valid() const {
        if (!account.valid() || !broker_margin_mode_is_netting(margin_mode) ||
            symbol.empty() || symbol.size() >= 64 || deal_ticket == 0 ||
            order_ticket == 0 || deal_position_id == 0 ||
            pre_position_identifier == 0 || post_position_identifier == 0 ||
            !valid_broker_position_direction(pre_direction) ||
            !valid_broker_position_direction(post_direction) ||
            !valid_broker_position_direction(deal_direction) ||
            !opposite_broker_position_directions(pre_direction, post_direction) ||
            deal_direction != post_direction || !pre_volume.valid() ||
            !post_volume.valid() || !deal_volume.valid() ||
            !broker_close_leg.valid() || !broker_reverse_open_leg.valid() ||
            pre_volume.scale != post_volume.scale ||
            pre_volume.scale != deal_volume.scale ||
            pre_volume.scale != broker_close_leg.scale ||
            pre_volume.scale != broker_reverse_open_leg.scale ||
            pre_volume.step_units != post_volume.step_units ||
            pre_volume.step_units != deal_volume.step_units ||
            pre_volume.step_units != broker_close_leg.step_units ||
            pre_volume.step_units != broker_reverse_open_leg.step_units ||
            !provenance.valid())
            return false;

        if (identity_relation == BrokerPositionIdentityRelation::continuous) {
            if (pre_position_identifier != post_position_identifier)
                return false;
        } else if (identity_relation == BrokerPositionIdentityRelation::replaced) {
            if (pre_position_identifier == post_position_identifier)
                return false;
        } else {
            return false;
        }

        if (deal_position_binding == BrokerDealPositionBinding::pre) {
            if (deal_position_id != pre_position_identifier)
                return false;
        } else if (deal_position_binding == BrokerDealPositionBinding::post) {
            if (deal_position_id != post_position_identifier)
                return false;
        } else {
            return false;
        }

        if (broker_close_leg.units != pre_volume.units ||
            broker_reverse_open_leg.units != post_volume.units ||
            deal_volume.units <= pre_volume.units ||
            deal_volume.units - pre_volume.units != post_volume.units ||
            broker_close_leg.units > deal_volume.units ||
            broker_reverse_open_leg.units > deal_volume.units)
            return false;
        if (broker_close_leg.units > (std::numeric_limits<std::uint64_t>::max)() -
                                         broker_reverse_open_leg.units ||
            broker_close_leg.units + broker_reverse_open_leg.units != deal_volume.units)
            return false;
        return true;
    }

    /// \brief Compares two records for idempotent replay.
    /// \param other Record to compare.
    /// \return True when every durable field matches exactly.
    bool operator==(const BrokerReversalRecord &other) const {
        return account == other.account && margin_mode == other.margin_mode &&
               symbol == other.symbol && deal_ticket == other.deal_ticket &&
               order_ticket == other.order_ticket &&
               deal_position_id == other.deal_position_id &&
               deal_position_binding == other.deal_position_binding &&
               pre_position_identifier == other.pre_position_identifier &&
               pre_direction == other.pre_direction && pre_volume == other.pre_volume &&
               post_position_identifier == other.post_position_identifier &&
               post_direction == other.post_direction &&
               post_volume == other.post_volume && deal_direction == other.deal_direction &&
               identity_relation == other.identity_relation &&
               deal_volume == other.deal_volume &&
               broker_close_leg == other.broker_close_leg &&
               broker_reverse_open_leg == other.broker_reverse_open_leg &&
               provenance == other.provenance;
    }
};

/// \brief Derives one broker decomposition from one fresh observation set.
/// \param observation Complete pre/post position and INOUT deal evidence.
/// \return A durable record, or empty when the observation is ambiguous or invalid.
inline std::optional<BrokerReversalRecord> derive_broker_reversal_record(
    const BrokerReversalObservation &observation) {
    BrokerReversalRecord record;
    record.account = observation.account;
    record.margin_mode = observation.margin_mode;
    record.symbol = observation.symbol;
    record.deal_ticket = observation.deal_ticket;
    record.order_ticket = observation.order_ticket;
    record.deal_position_id = observation.deal_position_id;
    record.deal_position_binding = observation.deal_position_binding;
    record.pre_position_identifier = observation.pre_position_identifier;
    record.pre_direction = observation.pre_direction;
    record.pre_volume = observation.pre_volume;
    record.post_position_identifier = observation.post_position_identifier;
    record.post_direction = observation.post_direction;
    record.post_volume = observation.post_volume;
    record.deal_direction = observation.deal_direction;
    record.identity_relation = observation.identity_relation;
    record.deal_volume = observation.deal_volume;
    record.broker_close_leg = observation.pre_volume;
    record.broker_reverse_open_leg = observation.post_volume;
    record.provenance = observation.provenance;
    return record.valid() ? std::optional<BrokerReversalRecord>(std::move(record))
                          : std::nullopt;
}

/// \enum BrokerReversalCommitStatus
/// \brief Reports the result of an immutable broker-record commit.
enum class BrokerReversalCommitStatus {
    committed,       ///< A new record was durably written.
    already_committed, ///< The exact same record already exists.
    conflict,        ///< The deal key already has a different decomposition.
    invalid_record,  ///< The candidate failed validation.
    io_error,        ///< Storage could not complete the operation.
};

/// \enum BrokerReversalLoadStatus
/// \brief Reports the result of loading one broker reversal.
enum class BrokerReversalLoadStatus {
    found,          ///< A complete valid record was loaded.
    not_found,      ///< No record exists for the requested key.
    invalid_record, ///< Storage exists but is malformed or inconsistent.
    io_error,       ///< Storage could not complete the read.
};

/// \struct BrokerReversalLoadResult
/// \brief Carries a status-bearing broker-record load result.
struct BrokerReversalLoadResult {
    BrokerReversalLoadStatus status = BrokerReversalLoadStatus::io_error;
    std::optional<BrokerReversalRecord> record;

    /// \brief Tests whether a complete record was loaded.
    /// \return True only when the status is `found` and a record is present.
    bool found() const {
        return status == BrokerReversalLoadStatus::found && record.has_value();
    }
};

/// \enum BrokerReversalScanStatus
/// \brief Reports the result of enumerating broker reversal records.
enum class BrokerReversalScanStatus {
    complete,       ///< Every broker reversal record was loaded and validated.
    invalid_record, ///< At least one record is malformed or inconsistent.
    io_error,       ///< Enumeration or a record read failed.
};

/// \struct BrokerReversalScanResult
/// \brief Carries an all-or-nothing broker-record scan result.
struct BrokerReversalScanResult {
    BrokerReversalScanStatus status = BrokerReversalScanStatus::io_error;
    std::vector<BrokerReversalRecord> records;

    /// \brief Tests whether the complete scan is usable.
    /// \return True only when all discovered records are valid.
    bool complete() const { return status == BrokerReversalScanStatus::complete; }
};

/// \class DurableBrokerReversalStore
/// \brief Persistence seam for immutable broker-level reversal proofs.
class DurableBrokerReversalStore {
public:
    virtual ~DurableBrokerReversalStore() = default;

    /// \brief Commits one immutable broker reversal durably.
    /// \param record Complete broker-level decomposition.
    /// \return Commit, idempotent replay, conflict, validation, or I/O status.
    virtual BrokerReversalCommitStatus commit(
        const BrokerReversalRecord &record) = 0;

    /// \brief Loads one broker reversal by account and deal ticket.
    /// \param key Immutable broker-record identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    virtual BrokerReversalLoadResult load(const BrokerReversalKey &key) const = 0;

    /// \brief Enumerates all broker reversal records for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    virtual BrokerReversalScanResult scan() const = 0;
};

} // namespace mt5bridge

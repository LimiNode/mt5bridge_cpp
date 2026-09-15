#pragma once

/// \file reconciliation.hpp
/// \brief Defines the observation-only C++ graph for typed MT5 evidence.

#include "trade.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

/// \struct AccountKey
/// \brief Immutable terminal identity used to scope observed trade evidence.
struct AccountKey {
    std::string server; ///< MT5 server name.
    std::uint64_t login = 0; ///< MT5 account login.

    /// \brief Tests whether the key has the minimum terminal identity.
    /// \return True when a server name is present.
    bool valid() const { return !server.empty(); }

    /// \brief Compares two account identities.
    /// \param other Key to compare.
    /// \return True when server and login are equal.
    bool operator==(const AccountKey &other) const {
        return login == other.login && server == other.server;
    }

    /// \brief Compares two account identities.
    /// \param other Key to compare.
    /// \return True when either identity component differs.
    bool operator!=(const AccountKey &other) const { return !(*this == other); }
};

/// \brief Builds an account key from a typed account observation.
/// \param info Account snapshot returned by the bridge.
/// \return Account key with a bounded copy of the server field.
inline AccountKey make_account_key(const Mt5AccountInfo &info) {
    AccountKey key;
    std::size_t length = 0;
    while (length < sizeof(info.server) && info.server[length] != '\0')
        ++length;
    key.server.assign(info.server, length);
    key.login = info.login;
    return key;
}

/// \enum ObservationApplyStatus
/// \brief Reports whether one observation batch was admitted to the graph.
enum class ObservationApplyStatus {
    accepted,          ///< Evidence was applied and the graph revision advanced.
    invalid_account,   ///< The batch did not contain a usable AccountKey.
    account_mismatch,  ///< The batch belonged to a different terminal account.
    invalid_evidence,  ///< A primary snapshot identifier was zero.
};

/// \struct ObservationBatch
/// \brief Groups one account-scoped, read-only snapshot observation.
struct ObservationBatch {
    AccountKey account; ///< Immutable terminal identity for every record below.
    std::vector<Mt5OrderSnapshot> active_orders; ///< Current active-order evidence.
    std::vector<Mt5PositionSnapshot> positions; ///< Current position evidence.
    std::vector<Mt5HistoryOrderSnapshot> history_orders; ///< Bounded history orders.
    std::vector<Mt5DealSnapshot> history_deals; ///< Bounded history deals.
};

/// \struct ObservationApplyResult
/// \brief Reports the graph revision after an observation attempt.
struct ObservationApplyResult {
    ObservationApplyStatus status = ObservationApplyStatus::invalid_account;
    std::uint64_t revision = 0; ///< Monotonic accepted-graph revision.

    /// \brief Tests whether the batch was admitted to the graph.
    /// \return True when the batch was accepted.
    bool accepted() const { return status == ObservationApplyStatus::accepted; }
};

/// \class ObservationGraph
/// \brief Stores account-scoped MT5 snapshots and exposes identity links.
///
/// This graph is deliberately observation-only. It never calls MetaTrader,
/// submits orders, infers a managed TradeId, or treats an array position as an
/// identity. A later journal/reconciliation layer may associate logical IDs
/// with this evidence, but raw MT5 tickets and position identifiers remain
/// separate here.
class ObservationGraph {
public:
    /// \brief Creates an unbound graph that adopts the first valid account.
    ObservationGraph() = default;

    /// \brief Creates a graph bound to one terminal account.
    /// \param account Immutable account identity to enforce.
    explicit ObservationGraph(AccountKey account)
        : account_(std::move(account)), bound_(account_.valid()) {}

    /// \brief Returns whether the graph has an account scope.
    /// \return True after construction with a valid key or the first accepted batch.
    bool bound() const { return bound_; }

    /// \brief Returns the graph account scope.
    /// \return Account key, or an empty key for an unbound graph.
    const AccountKey &account_key() const { return account_; }

    /// \brief Returns the latest accepted observation revision.
    /// \return Monotonically increasing revision number.
    std::uint64_t revision() const { return revision_; }

    /// \brief Applies one account-scoped snapshot atomically.
    /// \param batch Snapshot vectors and their immutable account identity.
    /// \return Admission status and the current graph revision.
    ObservationApplyResult apply(const ObservationBatch &batch) {
        if (!batch.account.valid())
            return {ObservationApplyStatus::invalid_account, revision_};
        if (bound_ && account_ != batch.account)
            return {ObservationApplyStatus::account_mismatch, revision_};
        if (!valid_primary_ids(batch))
            return {ObservationApplyStatus::invalid_evidence, revision_};

        if (!bound_) {
            account_ = batch.account;
            bound_ = true;
        }
        for (const auto &value : batch.active_orders)
            active_orders_[value.ticket] = value;
        for (const auto &value : batch.positions)
            positions_[value.ticket] = value;
        for (const auto &value : batch.history_orders)
            history_orders_[value.ticket] = value;
        for (const auto &value : batch.history_deals)
            history_deals_[value.ticket] = value;
        ++revision_;
        return {ObservationApplyStatus::accepted, revision_};
    }

    /// \brief Removes all evidence while retaining the account scope.
    /// \return New graph revision.
    std::uint64_t clear_evidence() {
        active_orders_.clear();
        positions_.clear();
        history_orders_.clear();
        history_deals_.clear();
        ++revision_;
        return revision_;
    }

    /// \brief Returns active orders sorted by MT5 ticket.
    /// \return Copy of the active-order evidence.
    std::vector<Mt5OrderSnapshot> active_orders() const {
        return values(active_orders_);
    }

    /// \brief Returns positions sorted by position ticket.
    /// \return Copy of the position evidence.
    std::vector<Mt5PositionSnapshot> positions() const { return values(positions_); }

    /// \brief Returns history orders sorted by MT5 ticket.
    /// \return Copy of the history-order evidence.
    std::vector<Mt5HistoryOrderSnapshot> history_orders() const {
        return values(history_orders_);
    }

    /// \brief Returns history deals sorted by MT5 ticket.
    /// \return Copy of the history-deal evidence.
    std::vector<Mt5DealSnapshot> history_deals() const {
        return values(history_deals_);
    }

    /// \brief Finds active and history orders linked to a position identifier.
    /// \param position_id MT5 ORDER_POSITION_ID/POSITION_IDENTIFIER value.
    /// \return Matching orders in deterministic ticket order.
    std::vector<Mt5OrderSnapshot> orders_for_position(std::uint64_t position_id) const {
        std::vector<Mt5OrderSnapshot> result;
        if (position_id == 0)
            return result;
        append_matching(active_orders_, position_id, &result);
        append_matching(history_orders_, position_id, &result);
        sort_by_ticket(&result);
        return result;
    }

    /// \brief Finds deals linked to one order ticket.
    /// \param order_ticket MT5 DEAL_ORDER value.
    /// \return Matching deals in deterministic ticket order.
    std::vector<Mt5DealSnapshot> deals_for_order(std::uint64_t order_ticket) const {
        return deals_matching([order_ticket](const Mt5DealSnapshot &value) {
            return order_ticket != 0 && value.order_ticket == order_ticket;
        });
    }

    /// \brief Finds deals linked to one position identifier.
    /// \param position_id MT5 DEAL_POSITION_ID value.
    /// \return Matching deals in deterministic ticket order.
    std::vector<Mt5DealSnapshot> deals_for_position(std::uint64_t position_id) const {
        return deals_matching([position_id](const Mt5DealSnapshot &value) {
            return position_id != 0 && value.position_id == position_id;
        });
    }

    /// \brief Finds positions retaining one POSITION_IDENTIFIER.
    /// \param identifier MT5 POSITION_IDENTIFIER value.
    /// \return Matching positions in deterministic ticket order.
    std::vector<Mt5PositionSnapshot> positions_for_identifier(
        std::uint64_t identifier) const {
        std::vector<Mt5PositionSnapshot> result;
        if (identifier == 0)
            return result;
        for (const auto &entry : positions_) {
            if (entry.second.identifier == identifier)
                result.push_back(entry.second);
        }
        return result;
    }

private:
    template <typename T>
    static std::vector<T> values(const std::map<std::uint64_t, T> &source) {
        std::vector<T> result;
        result.reserve(source.size());
        for (const auto &entry : source)
            result.push_back(entry.second);
        return result;
    }

    template <typename T>
    static void append_matching(const std::map<std::uint64_t, T> &source,
                                std::uint64_t position_id,
                                std::vector<T> *result) {
        for (const auto &entry : source) {
            if (entry.second.position_id == position_id)
                result->push_back(entry.second);
        }
    }

    template <typename T>
    static void sort_by_ticket(std::vector<T> *values_to_sort) {
        std::sort(values_to_sort->begin(), values_to_sort->end(),
                  [](const T &left, const T &right) { return left.ticket < right.ticket; });
    }

    template <typename Predicate>
    std::vector<Mt5DealSnapshot> deals_matching(Predicate predicate) const {
        std::vector<Mt5DealSnapshot> result;
        for (const auto &entry : history_deals_) {
            if (predicate(entry.second))
                result.push_back(entry.second);
        }
        return result;
    }

    static bool valid_primary_ids(const ObservationBatch &batch) {
        for (const auto &value : batch.active_orders)
            if (value.ticket == 0)
                return false;
        for (const auto &value : batch.positions)
            if (value.ticket == 0)
                return false;
        for (const auto &value : batch.history_orders)
            if (value.ticket == 0)
                return false;
        for (const auto &value : batch.history_deals)
            if (value.ticket == 0)
                return false;
        return true;
    }

    AccountKey account_;
    bool bound_ = false;
    std::uint64_t revision_ = 0;
    std::map<std::uint64_t, Mt5OrderSnapshot> active_orders_;
    std::map<std::uint64_t, Mt5PositionSnapshot> positions_;
    std::map<std::uint64_t, Mt5HistoryOrderSnapshot> history_orders_;
    std::map<std::uint64_t, Mt5DealSnapshot> history_deals_;
};

} // namespace mt5bridge

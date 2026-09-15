#pragma once

/// \file reconciliation.hpp
/// \brief Defines the observation-only C++ graph for typed MT5 evidence.

#include "trade.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

    /// \brief Tests whether the key has a complete terminal identity.
    /// \return True when server is non-empty and login is non-zero.
    bool valid() const { return !server.empty() && login != 0; }

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

/// \enum ObservationDomain
/// \brief Identifies namespaces covered by one observation batch.
enum class ObservationDomain : std::uint32_t {
    none = 0,
    active_orders = 1u << 0,  ///< Full account-wide active-order snapshot.
    positions = 1u << 1,      ///< Full account-wide position snapshot.
    history_orders = 1u << 2, ///< Bounded history-order evidence.
    history_deals = 1u << 3,  ///< Bounded history-deal evidence.
};

/// \brief Combines two observation-domain flags.
/// \param left First domain mask.
/// \param right Second domain mask.
/// \return Combined domain mask.
constexpr ObservationDomain operator|(ObservationDomain left, ObservationDomain right) {
    return static_cast<ObservationDomain>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

/// \brief Tests whether a domain is present in a mask.
/// \param mask Domain mask from an observation batch.
/// \param domain Domain to test.
/// \return True when the domain bit is set.
constexpr bool observes(ObservationDomain mask, ObservationDomain domain) {
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(domain)) != 0;
}

/// \struct ObservationWindow
/// \brief Inclusive millisecond coverage range for one history observation.
struct ObservationWindow {
    std::int64_t from_msc = 0; ///< Inclusive UTC range start.
    std::int64_t to_msc = -1; ///< Inclusive UTC range end.

    /// \brief Tests whether the range is representable and ordered.
    /// \return True for a non-negative inclusive range.
    bool valid() const { return from_msc >= 0 && to_msc >= from_msc; }
};

/// \enum ObservationApplyStatus
/// \brief Reports whether one observation batch was admitted to the graph.
enum class ObservationApplyStatus {
    accepted,          ///< Evidence was applied and the graph revision advanced.
    invalid_account,   ///< The batch did not contain a usable AccountKey.
    account_mismatch,  ///< The batch belonged to a different terminal account.
    invalid_evidence,  ///< Domain metadata, coverage, or primary IDs were invalid.
};

/// \struct ObservationBatch
/// \brief Groups one account-scoped, explicitly covered snapshot observation.
struct ObservationBatch {
    AccountKey account; ///< Immutable terminal identity for every record below.
    ObservationDomain observed_domains = ObservationDomain::none; ///< Complete domains below.
    std::optional<ObservationWindow> history_orders_window; ///< Optional complete-query coverage.
    std::optional<ObservationWindow> history_deals_window; ///< Optional complete-query coverage.
    std::vector<Mt5OrderSnapshot> active_orders; ///< Full active-order snapshot when observed.
    std::vector<Mt5PositionSnapshot> positions; ///< Full position snapshot when observed.
    std::vector<Mt5HistoryOrderSnapshot> history_orders; ///< Bounded history-order evidence.
    std::vector<Mt5DealSnapshot> history_deals; ///< Bounded history-deal evidence.
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
/// separate here. The graph is not thread-safe; callers must serialize reads
/// and writes through one owner loop or an external mutex.
class ObservationGraph {
public:
    /// \brief Creates an unbound graph that adopts the first valid account.
    ObservationGraph() = default;

    /// \brief Creates a graph bound to one terminal account.
    /// \param account Immutable account identity to enforce.
    explicit ObservationGraph(AccountKey account)
        : account_(account.valid() ? std::move(account) : AccountKey{}),
          bound_(account_.valid()) {}

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
    /// \param batch Snapshot vectors, coverage metadata, and account identity.
    /// \return Admission status and the current graph revision.
    /// \note If staging allocates successfully, all graph changes commit together.
    ObservationApplyResult apply(const ObservationBatch &batch) {
        if (!batch.account.valid())
            return {ObservationApplyStatus::invalid_account, revision_};
        if (bound_ && account_ != batch.account)
            return {ObservationApplyStatus::account_mismatch, revision_};
        if (!valid_batch_shape(batch))
            return {ObservationApplyStatus::invalid_evidence, revision_};

        const bool replace_active_orders =
            observes(batch.observed_domains, ObservationDomain::active_orders);
        const bool replace_positions =
            observes(batch.observed_domains, ObservationDomain::positions);
        const bool observe_history_orders =
            observes(batch.observed_domains, ObservationDomain::history_orders);
        const bool observe_history_deals =
            observes(batch.observed_domains, ObservationDomain::history_deals);

        AccountKey next_account;
        if (!bound_)
            next_account = batch.account;

        std::map<std::uint64_t, Mt5OrderSnapshot> next_active_orders;
        if (replace_active_orders && !make_unique_map(batch.active_orders, &next_active_orders))
            return {ObservationApplyStatus::invalid_evidence, revision_};

        std::map<std::uint64_t, Mt5PositionSnapshot> next_positions;
        if (replace_positions && !make_unique_map(batch.positions, &next_positions))
            return {ObservationApplyStatus::invalid_evidence, revision_};

        std::map<std::uint64_t, Mt5HistoryOrderSnapshot> next_history_orders;
        std::vector<ObservationWindow> next_history_order_coverage;
        if (observe_history_orders) {
            next_history_orders = history_orders_;
            next_history_order_coverage = history_order_coverage_;
            if (!merge_unique_map(batch.history_orders, &next_history_orders))
                return {ObservationApplyStatus::invalid_evidence, revision_};
            if (batch.history_orders_window)
                add_coverage(&next_history_order_coverage, *batch.history_orders_window);
        }

        std::map<std::uint64_t, Mt5DealSnapshot> next_history_deals;
        std::vector<ObservationWindow> next_history_deal_coverage;
        if (observe_history_deals) {
            next_history_deals = history_deals_;
            next_history_deal_coverage = history_deal_coverage_;
            if (!merge_unique_map(batch.history_deals, &next_history_deals))
                return {ObservationApplyStatus::invalid_evidence, revision_};
            if (batch.history_deals_window)
                add_coverage(&next_history_deal_coverage, *batch.history_deals_window);
        }

        if (!bound_) {
            account_.server.swap(next_account.server);
            std::swap(account_.login, next_account.login);
        }
        if (replace_active_orders)
            active_orders_.swap(next_active_orders);
        if (replace_positions)
            positions_.swap(next_positions);
        if (observe_history_orders) {
            history_orders_.swap(next_history_orders);
            history_order_coverage_.swap(next_history_order_coverage);
        }
        if (observe_history_deals) {
            history_deals_.swap(next_history_deals);
            history_deal_coverage_.swap(next_history_deal_coverage);
        }
        bound_ = true;
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
        history_order_coverage_.clear();
        history_deal_coverage_.clear();
        ++revision_;
        return revision_;
    }

    /// \brief Returns active orders sorted by MT5 ticket.
    /// \return Copy of the latest authoritative active-order snapshot.
    std::vector<Mt5OrderSnapshot> active_orders() const { return values(active_orders_); }

    /// \brief Returns positions sorted by position ticket.
    /// \return Copy of the latest authoritative position snapshot.
    std::vector<Mt5PositionSnapshot> positions() const { return values(positions_); }

    /// \brief Returns history orders sorted by MT5 ticket.
    /// \return Copy of accumulated history-order evidence.
    std::vector<Mt5HistoryOrderSnapshot> history_orders() const {
        return values(history_orders_);
    }

    /// \brief Returns history deals sorted by MT5 ticket.
    /// \return Copy of accumulated history-deal evidence.
    std::vector<Mt5DealSnapshot> history_deals() const {
        return values(history_deals_);
    }

    /// \brief Returns merged history-order coverage windows.
    /// \return Copy of inclusive ranges queried for history orders.
    std::vector<ObservationWindow> history_orders_coverage() const {
        return history_order_coverage_;
    }

    /// \brief Returns merged history-deal coverage windows.
    /// \return Copy of inclusive ranges queried for history deals.
    std::vector<ObservationWindow> history_deals_coverage() const {
        return history_deal_coverage_;
    }

    /// \brief Finds active orders linked to a position identifier.
    /// \param position_id MT5 ORDER_POSITION_ID/POSITION_IDENTIFIER value.
    /// \return Matching active orders in deterministic ticket order.
    std::vector<Mt5OrderSnapshot> active_orders_for_position(
        std::uint64_t position_id) const {
        return orders_matching(active_orders_, position_id);
    }

    /// \brief Finds history orders linked to a position identifier.
    /// \param position_id MT5 ORDER_POSITION_ID/POSITION_IDENTIFIER value.
    /// \return Matching history orders in deterministic ticket order.
    std::vector<Mt5HistoryOrderSnapshot> history_orders_for_position(
        std::uint64_t position_id) const {
        return orders_matching(history_orders_, position_id);
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
    /// \brief Bit mask of all currently supported observation domains.
    static constexpr std::uint32_t kKnownDomainBits =
        static_cast<std::uint32_t>(ObservationDomain::active_orders) |
        static_cast<std::uint32_t>(ObservationDomain::positions) |
        static_cast<std::uint32_t>(ObservationDomain::history_orders) |
        static_cast<std::uint32_t>(ObservationDomain::history_deals);

    template <typename T>
    static std::vector<T> values(const std::map<std::uint64_t, T> &source) {
        std::vector<T> result;
        result.reserve(source.size());
        for (const auto &entry : source)
            result.push_back(entry.second);
        return result;
    }

    template <typename T>
    static std::vector<T> orders_matching(const std::map<std::uint64_t, T> &source,
                                           std::uint64_t position_id) {
        std::vector<T> result;
        if (position_id == 0)
            return result;
        for (const auto &entry : source) {
            if (entry.second.position_id == position_id)
                result.push_back(entry.second);
        }
        return result;
    }

    template <typename T>
    static bool make_unique_map(const std::vector<T> &source,
                                std::map<std::uint64_t, T> *target) {
        target->clear();
        for (const auto &value : source) {
            if (value.ticket == 0 || !target->emplace(value.ticket, value).second)
                return false;
        }
        return true;
    }

    template <typename T>
    static bool merge_unique_map(const std::vector<T> &source,
                                 std::map<std::uint64_t, T> *target) {
        std::map<std::uint64_t, T> incoming;
        if (!make_unique_map(source, &incoming))
            return false;
        for (const auto &entry : incoming)
            (*target)[entry.first] = entry.second;
        return true;
    }

    static bool records_in_window(const ObservationBatch &batch) {
        if (batch.history_orders_window) {
            for (const auto &value : batch.history_orders) {
                if (value.time_done_msc < batch.history_orders_window->from_msc ||
                    value.time_done_msc > batch.history_orders_window->to_msc)
                    return false;
            }
        }
        if (batch.history_deals_window) {
            for (const auto &value : batch.history_deals) {
                if (value.time_msc < batch.history_deals_window->from_msc ||
                    value.time_msc > batch.history_deals_window->to_msc)
                    return false;
            }
        }
        return true;
    }

    static bool valid_batch_shape(const ObservationBatch &batch) {
        const auto mask = static_cast<std::uint32_t>(batch.observed_domains);
        if (mask == 0 || (mask & ~kKnownDomainBits) != 0)
            return false;
        const bool active_orders = observes(batch.observed_domains, ObservationDomain::active_orders);
        const bool positions = observes(batch.observed_domains, ObservationDomain::positions);
        const bool history_orders = observes(batch.observed_domains, ObservationDomain::history_orders);
        const bool history_deals = observes(batch.observed_domains, ObservationDomain::history_deals);
        if ((!active_orders && !batch.active_orders.empty()) ||
            (!positions && !batch.positions.empty()) ||
            (!history_orders && (!batch.history_orders.empty() || batch.history_orders_window)) ||
            (!history_deals && (!batch.history_deals.empty() || batch.history_deals_window)))
            return false;
        if (history_orders && batch.history_orders_window &&
            !batch.history_orders_window->valid())
            return false;
        if (history_deals && batch.history_deals_window &&
            !batch.history_deals_window->valid())
            return false;
        if (history_orders && batch.history_orders.empty() && !batch.history_orders_window)
            return false;
        if (history_deals && batch.history_deals.empty() && !batch.history_deals_window)
            return false;
        return records_in_window(batch);
    }

    static void add_coverage(std::vector<ObservationWindow> *coverage,
                             ObservationWindow window) {
        coverage->push_back(window);
        std::sort(coverage->begin(), coverage->end(),
                  [](const ObservationWindow &left, const ObservationWindow &right) {
                      if (left.from_msc != right.from_msc)
                          return left.from_msc < right.from_msc;
                      return left.to_msc < right.to_msc;
                  });
        std::vector<ObservationWindow> merged;
        merged.reserve(coverage->size());
        for (const auto &current : *coverage) {
            if (merged.empty() || current.from_msc > merged.back().to_msc) {
                merged.push_back(current);
            } else if (current.to_msc > merged.back().to_msc) {
                merged.back().to_msc = current.to_msc;
            }
        }
        coverage->swap(merged);
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

    AccountKey account_;
    bool bound_ = false;
    std::uint64_t revision_ = 0;
    std::map<std::uint64_t, Mt5OrderSnapshot> active_orders_;
    std::map<std::uint64_t, Mt5PositionSnapshot> positions_;
    std::map<std::uint64_t, Mt5HistoryOrderSnapshot> history_orders_;
    std::map<std::uint64_t, Mt5DealSnapshot> history_deals_;
    std::vector<ObservationWindow> history_order_coverage_;
    std::vector<ObservationWindow> history_deal_coverage_;
};

} // namespace mt5bridge

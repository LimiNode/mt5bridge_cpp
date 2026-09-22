#pragma once

/// \file observation/environment_consistency.hpp
/// \brief Defines bounded cross-view consistency checks for MT5 observations.

#include <mt5bridge/observation/coordinator.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

/// \enum EnvironmentConsistencyState
/// \brief Describes the result of a bounded cross-view observation cycle.
enum class EnvironmentConsistencyState {
    consistent,             ///< Consecutive observations agree on identity links.
    awaiting_confirmation,  ///< Evidence is plausible but needs another observation.
    cross_view_mismatch,    ///< Observed records contain a contradictory link.
    account_changed,        ///< The account identity changed between observations.
    unstable_environment,   ///< Bounded observations kept changing without settling.
    insufficient_evidence,  ///< Required domains or graph fields were not usable.
    invalid_request,        ///< The policy request itself is malformed.
};

struct EnvironmentConsistencyRequest;

/// \class EnvironmentConsistencyProof
/// \brief Opaque scope- and revision-bound proof produced by a consistent policy run.
///
/// A proof is tied to one observation graph instance and its exact latest
/// revision and records the domains and history windows that were checked.
/// Callers may copy the value for a synchronous admission attempt, but cannot
/// manufacture a valid proof from public fields.
class EnvironmentConsistencyProof {
public:
    EnvironmentConsistencyProof(const EnvironmentConsistencyProof &) = default;
    EnvironmentConsistencyProof &operator=(const EnvironmentConsistencyProof &) = default;
    EnvironmentConsistencyProof(EnvironmentConsistencyProof &&) = default;
    EnvironmentConsistencyProof &operator=(EnvironmentConsistencyProof &&) = default;

    /// \brief Tests whether the proof carries complete provenance and scope.
    /// \return True only for a valid account, graph identity, revision, and scope.
    bool valid() const {
        constexpr std::uint32_t supported_domains =
            static_cast<std::uint32_t>(ObservationDomain::active_orders) |
            static_cast<std::uint32_t>(ObservationDomain::positions) |
            static_cast<std::uint32_t>(ObservationDomain::history_orders) |
            static_cast<std::uint32_t>(ObservationDomain::history_deals);
        const auto domains = static_cast<std::uint32_t>(observed_domains_);
        return account_.valid() && graph_instance_id_ != 0 && last_graph_revision_ != 0 &&
               domains != 0 && (domains & ~supported_domains) == 0 &&
               (observes(observed_domains_, ObservationDomain::history_orders) ==
                history_orders_window_.has_value()) &&
               (observes(observed_domains_, ObservationDomain::history_deals) ==
                history_deals_window_.has_value()) &&
               (!history_orders_window_ || history_orders_window_->valid()) &&
               (!history_deals_window_ || history_deals_window_->valid());
    }

    /// \brief Returns the account covered by the proof.
    /// \return Immutable account identity.
    const AccountKey &account() const { return account_; }

    /// \brief Returns the observation graph provenance identity.
    /// \return Process-local graph instance ID.
    std::uint64_t graph_instance_id() const { return graph_instance_id_; }

    /// \brief Returns the exact latest revision covered by the proof.
    /// \return Graph revision at the final coherent sample.
    std::uint64_t last_graph_revision() const { return last_graph_revision_; }

    /// \brief Returns the domains covered by the proof.
    /// \return Immutable observation-domain mask.
    ObservationDomain observed_domains() const { return observed_domains_; }

    /// \brief Returns the history-order window covered by the proof.
    /// \return Inclusive window, or empty when history orders were not observed.
    const std::optional<ObservationWindow> &history_orders_window() const {
        return history_orders_window_;
    }

    /// \brief Returns the history-deal window covered by the proof.
    /// \return Inclusive window, or empty when history deals were not observed.
    const std::optional<ObservationWindow> &history_deals_window() const {
        return history_deals_window_;
    }

    /// \brief Tests whether the proof covers a requested admission scope.
    /// \param required_scope Domains and history range required by admission.
    /// \return True when every requested domain and range is covered.
    bool covers(const EnvironmentConsistencyRequest &required_scope) const;

private:
    EnvironmentConsistencyProof() = default;

    static EnvironmentConsistencyProof create(
        const AccountKey &account, std::uint64_t graph_instance_id,
        std::uint64_t last_graph_revision, ObservationDomain observed_domains,
        std::optional<ObservationWindow> history_orders_window,
        std::optional<ObservationWindow> history_deals_window) {
        EnvironmentConsistencyProof proof;
        proof.account_ = account;
        proof.graph_instance_id_ = graph_instance_id;
        proof.last_graph_revision_ = last_graph_revision;
        proof.observed_domains_ = observed_domains;
        proof.history_orders_window_ = std::move(history_orders_window);
        proof.history_deals_window_ = std::move(history_deals_window);
        return proof;
    }

    AccountKey account_;
    std::uint64_t graph_instance_id_ = 0;
    std::uint64_t last_graph_revision_ = 0;
    ObservationDomain observed_domains_ = ObservationDomain::none;
    std::optional<ObservationWindow> history_orders_window_;
    std::optional<ObservationWindow> history_deals_window_;

    friend class EnvironmentConsistencyPolicy;
};

/// \struct EnvironmentConsistencyRequest
/// \brief Selects the domains and bounded confirmation budget for one cycle.
struct EnvironmentConsistencyRequest {
    bool require_active_orders = true; ///< Require a complete active-order view.
    bool require_positions = true; ///< Require a complete current-position view.
    std::optional<ObservationWindow> history_orders_window; ///< Required order history.
    std::optional<ObservationWindow> history_deals_window; ///< Required deal history.
    std::size_t max_observations = 2; ///< Maximum observations in this bounded cycle.

    /// \brief Maximum number of collections accepted by the policy.
    static constexpr std::size_t kMaximumObservations = 8;

    /// \brief Tests whether the request has a bounded, non-empty scope.
    /// \return True when at least one domain and valid windows are supplied.
    bool valid() const {
        const bool has_domain = require_active_orders || require_positions ||
                                history_orders_window.has_value() ||
                                history_deals_window.has_value();
        return has_domain && max_observations >= 2 &&
               max_observations <= kMaximumObservations &&
               (!history_orders_window || history_orders_window->valid()) &&
               (!history_deals_window || history_deals_window->valid());
    }

    /// \brief Returns the exact domain mask required from every observation.
    /// \return Observation domains selected by this request.
    ObservationDomain required_domains() const {
        ObservationDomain domains = ObservationDomain::none;
        if (require_active_orders)
            domains = domains | ObservationDomain::active_orders;
        if (require_positions)
            domains = domains | ObservationDomain::positions;
        if (history_orders_window)
            domains = domains | ObservationDomain::history_orders;
        if (history_deals_window)
            domains = domains | ObservationDomain::history_deals;
        return domains;
    }
};

inline bool EnvironmentConsistencyProof::covers(
    const EnvironmentConsistencyRequest &required_scope) const {
    if (!valid() || !required_scope.valid())
        return false;

    const auto proof_domains = static_cast<std::uint32_t>(observed_domains_);
    const auto required_domains =
        static_cast<std::uint32_t>(required_scope.required_domains());
    if ((proof_domains & required_domains) != required_domains)
        return false;

    const auto covers_window = [](const std::optional<ObservationWindow> &proof_window,
                                  const std::optional<ObservationWindow> &required_window) {
        if (!required_window)
            return true;
        return proof_window && proof_window->from_msc <= required_window->from_msc &&
               proof_window->to_msc >= required_window->to_msc;
    };
    return covers_window(history_orders_window_, required_scope.history_orders_window) &&
           covers_window(history_deals_window_, required_scope.history_deals_window);
}

/// \struct EnvironmentConsistencyResult
/// \brief Reports a bounded environment-consistency decision.
struct EnvironmentConsistencyResult {
    EnvironmentConsistencyState state = EnvironmentConsistencyState::invalid_request;
    AccountKey account; ///< Account shared by all accepted observations.
    std::optional<EnvironmentConsistencyProof> proof; ///< Present only when consistent.
    std::size_t observation_count = 0; ///< Number of observations evaluated.
    std::size_t unresolved_links = 0; ///< Links still awaiting another view.
    std::size_t contradictory_links = 0; ///< Directly conflicting links found.
    std::size_t insufficient_links = 0; ///< Links outside the requested evidence scope.

    /// \brief Tests whether the environment may enter a later dispatch layer.
    /// \return True only after two consecutive coherent observations agree and
    /// a revision-bound proof is available.
    bool consistent() const {
        return state == EnvironmentConsistencyState::consistent && proof.has_value() &&
               proof->valid();
    }
};

/// \class EnvironmentConsistencyPolicy
/// \brief Evaluates bounded cross-view evidence without runtime side effects.
///
/// The policy deliberately does not claim that MT5 supplied an atomic snapshot.
/// It checks account provenance, the requested observation shape, and links that
/// are visible in the supplied views. Missing links are treated as provisional
/// when MT5 can legitimately publish the related records at different times.
/// Callers provide one or more sequential samples and own the retry cadence.
class EnvironmentConsistencyPolicy {
public:
    /// \brief Evaluates a bounded sequence of observations.
    /// \param observations Sequential samples accepted by one coordinator graph.
    /// \param request Required domains and confirmation budget.
    /// \return Fail-closed consistency state; no runtime call is made.
    static EnvironmentConsistencyResult evaluate(
        const std::vector<ObservationSample> &observations,
        const EnvironmentConsistencyRequest &request) {
        EnvironmentConsistencyResult result;
        result.observation_count = observations.size();
        if (!request.valid())
            return result;
        if (observations.empty() || observations.size() > request.max_observations) {
            result.state = EnvironmentConsistencyState::insufficient_evidence;
            return result;
        }

        const auto &first = observations.front();
        if (first.graph_instance_id() == 0 || first.graph_revision() == 0) {
            result.state = EnvironmentConsistencyState::insufficient_evidence;
            return result;
        }
        const auto graph_instance_id = first.graph_instance_id();
        if (!first.batch().account.valid()) {
            result.state = EnvironmentConsistencyState::insufficient_evidence;
            return result;
        }
        result.account = first.batch().account;

        std::vector<BatchAnalysis> analyses;
        analyses.reserve(observations.size());
        std::uint64_t previous_revision = 0;
        for (const auto &sample : observations) {
            const auto &observation = sample.batch();
            if (!observation.account.valid()) {
                result.state = EnvironmentConsistencyState::insufficient_evidence;
                return result;
            }
            if (observation.account != result.account) {
                result.state = EnvironmentConsistencyState::account_changed;
                return result;
            }
            if (sample.graph_instance_id() != graph_instance_id ||
                sample.graph_revision() == 0 ||
                (previous_revision != 0 &&
                 (previous_revision == (std::numeric_limits<std::uint64_t>::max)() ||
                  sample.graph_revision() != previous_revision + 1))) {
                result.state = EnvironmentConsistencyState::insufficient_evidence;
                return result;
            }
            previous_revision = sample.graph_revision();
            if (!matches_request(observation, request)) {
                result.state = EnvironmentConsistencyState::insufficient_evidence;
                return result;
            }
            const auto analysis = analyze(observation, request);
            result.unresolved_links = analysis.unresolved_links;
            result.contradictory_links += analysis.contradictory_links;
            result.insufficient_links += analysis.insufficient_links;
            if (analysis.contradictory_links != 0) {
                result.state = EnvironmentConsistencyState::cross_view_mismatch;
                return result;
            }
            analyses.push_back(analysis);
        }

        if (result.insufficient_links != 0) {
            result.state = EnvironmentConsistencyState::insufficient_evidence;
            return result;
        }

        const auto &latest = analyses.back();
        if (latest.unresolved_links != 0) {
            result.state = analyses.size() == request.max_observations
                               ? EnvironmentConsistencyState::unstable_environment
                               : EnvironmentConsistencyState::awaiting_confirmation;
            return result;
        }

        if (analyses.size() >= 2) {
            const auto &previous = analyses[analyses.size() - 2];
            if (previous.unresolved_links == 0 && previous.signature == latest.signature) {
                result.state = EnvironmentConsistencyState::consistent;
                result.proof = EnvironmentConsistencyProof::create(
                    result.account, graph_instance_id, observations.back().graph_revision(),
                    request.required_domains(), request.history_orders_window,
                    request.history_deals_window);
                return result;
            }
        }

        std::size_t coherent_count = 0;
        for (const auto &analysis : analyses) {
            if (analysis.unresolved_links == 0)
                ++coherent_count;
        }
        if (analyses.size() == request.max_observations && coherent_count >= 2) {
            result.state = EnvironmentConsistencyState::unstable_environment;
        } else {
            result.state = EnvironmentConsistencyState::awaiting_confirmation;
        }
        return result;
    }

private:
    using SignatureItem = std::array<std::uint64_t, 5>;

    struct BatchAnalysis {
        std::vector<SignatureItem> signature;
        std::size_t unresolved_links = 0;
        std::size_t contradictory_links = 0;
        std::size_t insufficient_links = 0;
    };

    static bool same_window(const std::optional<ObservationWindow> &left,
                            const std::optional<ObservationWindow> &right) {
        if (left.has_value() != right.has_value())
            return false;
        return !left || (left->from_msc == right->from_msc &&
                         left->to_msc == right->to_msc);
    }

    static bool has_field(std::uint64_t known_fields, std::uint64_t field) {
        return (known_fields & field) == field;
    }

    static bool in_window(std::int64_t value, const ObservationWindow &window) {
        return value >= window.from_msc && value <= window.to_msc;
    }

    static bool valid_orders(const std::vector<Mt5OrderSnapshot> &values,
                             std::uint64_t required_fields,
                             const std::optional<ObservationWindow> &window) {
        std::map<std::uint64_t, bool> tickets;
        for (const auto &value : values) {
            if (value.ticket == 0 || !has_field(value.known_fields, required_fields) ||
                !tickets.emplace(value.ticket, true).second)
                return false;
            if (window && !in_window(value.time_done_msc, *window))
                return false;
        }
        return true;
    }

    static bool valid_positions(const std::vector<Mt5PositionSnapshot> &values) {
        std::map<std::uint64_t, bool> tickets;
        for (const auto &value : values) {
            if (value.ticket == 0 ||
                !has_field(value.known_fields,
                           MT5BRIDGE_POSITION_KNOWN_TICKET |
                               MT5BRIDGE_POSITION_KNOWN_IDENTIFIER) ||
                !tickets.emplace(value.ticket, true).second)
                return false;
        }
        return true;
    }

    static bool valid_deals(const std::vector<Mt5DealSnapshot> &values,
                            const std::optional<ObservationWindow> &window) {
        std::map<std::uint64_t, bool> tickets;
        constexpr std::uint64_t required_fields =
            MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
            MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TIME;
        for (const auto &value : values) {
            if (value.ticket == 0 || !has_field(value.known_fields, required_fields) ||
                !tickets.emplace(value.ticket, true).second)
                return false;
            if (window && !in_window(value.time_msc, *window))
                return false;
        }
        return true;
    }

    static bool matches_request(const ObservationBatch &batch,
                                const EnvironmentConsistencyRequest &request) {
        if (batch.observed_domains != request.required_domains())
            return false;
        if (!same_window(batch.history_orders_window, request.history_orders_window) ||
            !same_window(batch.history_deals_window, request.history_deals_window))
            return false;
        if ((!request.require_active_orders && !batch.active_orders.empty()) ||
            (!request.require_positions && !batch.positions.empty()) ||
            (!request.history_orders_window &&
             (!batch.history_orders.empty() || batch.history_orders_window)) ||
            (!request.history_deals_window &&
             (!batch.history_deals.empty() || batch.history_deals_window)))
            return false;

        constexpr std::uint64_t order_fields =
            MT5BRIDGE_ORDER_KNOWN_TICKET | MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
        const bool active_valid =
            !request.require_active_orders || valid_orders(batch.active_orders, order_fields,
                                                            std::nullopt);
        const auto history_order_fields = order_fields | MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
        const bool history_orders_valid =
            !request.history_orders_window ||
            valid_orders(batch.history_orders, history_order_fields,
                         request.history_orders_window);
        return active_valid &&
               (!request.require_positions || valid_positions(batch.positions)) &&
               history_orders_valid &&
               (!request.history_deals_window ||
                valid_deals(batch.history_deals, request.history_deals_window));
    }

    static BatchAnalysis analyze(const ObservationBatch &batch,
                                 const EnvironmentConsistencyRequest &request) {
        BatchAnalysis result;
        std::map<std::uint64_t, std::uint64_t> position_identifier_to_ticket;
        std::map<std::uint64_t, std::uint64_t> active_order_positions;
        std::map<std::uint64_t, std::uint64_t> history_order_positions;
        for (const auto &value : batch.positions) {
            if (value.identifier == 0)
                continue;
            const auto inserted = position_identifier_to_ticket.emplace(value.identifier,
                                                                         value.ticket);
            if (!inserted.second && inserted.first->second != value.ticket)
                ++result.contradictory_links;
            result.signature.push_back({2, value.ticket, value.identifier, 0, 0});
        }
        for (const auto &value : batch.active_orders) {
            active_order_positions.emplace(value.ticket, value.position_id);
            const bool has_position_by_id =
                has_field(value.known_fields, MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID);
            result.signature.push_back(
                {1, value.ticket, value.position_id,
                 has_position_by_id ? value.position_by_id : 0u,
                 has_position_by_id ? 1u : 0u});
            if (request.require_positions && value.position_id != 0 &&
                position_identifier_to_ticket.find(value.position_id) ==
                    position_identifier_to_ticket.end())
                ++result.unresolved_links;
            if (request.require_positions &&
                has_field(value.known_fields, MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID) &&
                value.position_by_id != 0 &&
                position_identifier_to_ticket.find(value.position_by_id) ==
                    position_identifier_to_ticket.end())
                ++result.unresolved_links;
        }
        for (const auto &value : batch.history_orders) {
            history_order_positions.emplace(value.ticket, value.position_id);
            result.signature.push_back({3, value.ticket, value.position_id, 0, 0});
        }
        for (const auto &value : batch.history_deals) {
            result.signature.push_back({4, value.ticket, value.order_ticket,
                                        value.position_id, 0});
            if (value.order_ticket != 0 &&
                (observes(batch.observed_domains, ObservationDomain::history_orders) ||
                 observes(batch.observed_domains, ObservationDomain::active_orders))) {
                const auto history_order = history_order_positions.find(value.order_ticket);
                const auto active_order = active_order_positions.find(value.order_ticket);
                if (history_order == history_order_positions.end() &&
                    active_order == active_order_positions.end() &&
                    observes(batch.observed_domains, ObservationDomain::history_orders) &&
                    observes(batch.observed_domains, ObservationDomain::active_orders)) {
                    if (!batch.history_orders_window || !batch.history_deals_window ||
                        !in_window(value.time_msc, *batch.history_orders_window))
                        ++result.insufficient_links;
                    else
                        ++result.unresolved_links;
                } else if (history_order == history_order_positions.end() &&
                           active_order == active_order_positions.end()) {
                    ++result.insufficient_links;
                } else {
                    if (history_order != history_order_positions.end() &&
                        history_order->second != 0 && value.position_id != 0 &&
                        history_order->second != value.position_id)
                        ++result.contradictory_links;
                    if (active_order != active_order_positions.end() &&
                        active_order->second != 0 && value.position_id != 0 &&
                        active_order->second != value.position_id)
                        ++result.contradictory_links;
                }
            } else if (value.order_ticket != 0) {
                ++result.insufficient_links;
            }
        }
        for (const auto &entry : active_order_positions) {
            const auto history_order = history_order_positions.find(entry.first);
            if (history_order != history_order_positions.end() && entry.second != 0 &&
                history_order->second != 0 && entry.second != history_order->second)
                ++result.contradictory_links;
        }
        std::sort(result.signature.begin(), result.signature.end());
        return result;
    }
};

} // namespace mt5bridge

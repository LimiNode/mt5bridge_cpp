#pragma once

/// \file reconciliation/engine.hpp
/// \brief Defines observation-only reconciliation predicates over the graph.

#include <mt5bridge/observation/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

/// \enum ReconciliationOutcome
/// \brief Describes the result of evaluating observation predicates.
enum class ReconciliationOutcome {
    pending,          ///< Required post-baseline observations are not ready.
    confirmed,        ///< Every predicate has matching post-baseline evidence.
    not_observed,     ///< The deadline elapsed without satisfying every predicate.
    account_mismatch, ///< The graph and baseline do not share one account scope.
    trade_event_gap,  ///< An incomplete event hint stream still lacks fresh snapshots.
    ambiguous,        ///< Evidence is contradictory or the request is invalid.
};

/// \enum ReconciliationReason
/// \brief Explains the state selected by the predicate evaluator.
enum class ReconciliationReason {
    none,                    ///< No additional diagnostic reason.
    waiting_for_observation, ///< Fresh evidence or a caller-owned deadline is pending.
    evidence_missing,        ///< The caller's deadline elapsed without a match.
    contradictory_evidence,  ///< Evidence is unusable or mutually inconsistent.
    account_mismatch,        ///< Account identities differ.
    graph_mismatch,          ///< Baseline belongs to another graph instance.
    unresolved_operation,     ///< A prior operation remains unresolved.
    trade_event_gap,         ///< Event hints were incomplete before fresh snapshots.
    invalid_request,         ///< Predicate, baseline, or account input is malformed.
};

/// \enum ReconciliationPredicateKind
/// \brief Identifies one explicit presence or absence assertion.
enum class ReconciliationPredicateKind {
    active_order_present,  ///< An active order ticket must be present.
    active_order_absent,   ///< An active order ticket must be absent.
    position_present,      ///< A position ticket must be present.
    position_absent,       ///< A position ticket must be absent.
    history_order_present, ///< A history order ticket must be observed.
    history_order_absent,  ///< A history order ticket must be absent in a window.
    history_deal_present,  ///< A history deal ticket must be observed.
    history_deal_absent,   ///< A history deal ticket must be absent in a window.
};

/// \enum ReconciliationTransition
/// \brief Describes the causal state change expected after dispatch.
enum class ReconciliationTransition {
    unspecified,       ///< A live observation request has no durable transition contract.
    absent_to_present, ///< The identity was absent before dispatch and must appear.
    present_to_absent, ///< The identity existed before dispatch and must disappear.
};

/// \class ReconciliationBaseline
/// \brief Immutable graph revisions captured before an observation cycle.
class ReconciliationBaseline {
public:
    /// \brief Returns the account scope captured with the revisions.
    /// \return Immutable account identity.
    const AccountKey &account() const { return account_; }

    /// \brief Returns the global graph revision at capture time.
    /// \return Global graph revision.
    std::uint64_t graph_revision() const { return graph_revision_; }

    /// \brief Returns the graph provenance captured with the revisions.
    /// \return Process-local graph instance identity.
    std::uint64_t graph_instance_id() const { return graph_instance_id_; }

    /// \brief Returns the active-order domain revision at capture time.
    /// \return Active-order domain revision.
    std::uint64_t active_orders_revision() const { return active_orders_revision_; }

    /// \brief Returns the position domain revision at capture time.
    /// \return Position domain revision.
    std::uint64_t positions_revision() const { return positions_revision_; }

    /// \brief Returns the history-order domain revision at capture time.
    /// \return History-order domain revision.
    std::uint64_t history_orders_revision() const { return history_orders_revision_; }

    /// \brief Returns the history-deal domain revision at capture time.
    /// \return History-deal domain revision.
    std::uint64_t history_deals_revision() const { return history_deals_revision_; }

    /// \brief Restores a previously durable baseline without creating proof.
    /// \param account Account scope captured with the revisions.
    /// \param graph_instance_id Graph provenance identity.
    /// \param graph_revision Global graph revision.
    /// \param active_orders_revision Active-order domain revision.
    /// \param positions_revision Position domain revision.
    /// \param history_orders_revision History-order domain revision.
    /// \param history_deals_revision History-deal domain revision.
    /// \return Valid baseline, or empty when durable fields are malformed.
    static std::optional<ReconciliationBaseline> restore(
        AccountKey account, std::uint64_t graph_instance_id,
        std::uint64_t graph_revision, std::uint64_t active_orders_revision,
        std::uint64_t positions_revision, std::uint64_t history_orders_revision,
        std::uint64_t history_deals_revision) {
        ReconciliationBaseline baseline(
            std::move(account), graph_instance_id, graph_revision,
            active_orders_revision, positions_revision, history_orders_revision,
            history_deals_revision);
        return baseline.valid() ? std::optional<ReconciliationBaseline>(std::move(baseline))
                                : std::nullopt;
    }

    /// \brief Tests whether this baseline was captured from a valid graph.
    /// \return True only for an account-bound, internally ordered snapshot.
    bool valid() const {
        return account_.valid() && graph_instance_id_ != 0 &&
               active_orders_revision_ <= graph_revision_ &&
               positions_revision_ <= graph_revision_ &&
               history_orders_revision_ <= graph_revision_ &&
               history_deals_revision_ <= graph_revision_;
    }

private:
    friend ReconciliationBaseline capture_reconciliation_baseline(
        const ObservationGraph &graph);

    ReconciliationBaseline(AccountKey account, std::uint64_t graph_instance_id,
                           std::uint64_t graph_revision,
                           std::uint64_t active_orders_revision,
                           std::uint64_t positions_revision,
                           std::uint64_t history_orders_revision,
                           std::uint64_t history_deals_revision)
        : account_(std::move(account)),
          graph_instance_id_(graph_instance_id),
          graph_revision_(graph_revision),
          active_orders_revision_(active_orders_revision),
          positions_revision_(positions_revision),
          history_orders_revision_(history_orders_revision),
          history_deals_revision_(history_deals_revision) {}

    AccountKey account_;
    std::uint64_t graph_instance_id_ = 0;
    std::uint64_t graph_revision_ = 0;
    std::uint64_t active_orders_revision_ = 0;
    std::uint64_t positions_revision_ = 0;
    std::uint64_t history_orders_revision_ = 0;
    std::uint64_t history_deals_revision_ = 0;
};

/// \brief Captures all current graph revisions for a later reconciliation.
/// \param graph Observation graph owned by the caller.
/// \return Immutable baseline snapshot; an unbound graph produces an invalid account.
inline ReconciliationBaseline capture_reconciliation_baseline(
    const ObservationGraph &graph) {
    return ReconciliationBaseline(
        graph.account_key(), graph.instance_id(), graph.revision(),
        graph.domain_revision(ObservationDomain::active_orders),
        graph.domain_revision(ObservationDomain::positions),
        graph.domain_revision(ObservationDomain::history_orders),
        graph.domain_revision(ObservationDomain::history_deals));
}

/// \struct ReconciliationPredicate
/// \brief Describes one explicit evidence assertion.
struct ReconciliationPredicate {
    ReconciliationPredicateKind kind = ReconciliationPredicateKind::active_order_present;
    std::uint64_t ticket = 0; ///< Primary MT5 ticket asserted by the predicate.
    /// \brief Optional time window for a history predicate.
    /// \note Required for history absence and optional for history presence.
    std::optional<ObservationWindow> history_window;
    /// \brief Whether the asserted identity existed in the pre-dispatch baseline.
    /// \note Required when this predicate is persisted in a durable descriptor.
    std::optional<bool> baseline_present;
    /// \brief Correlation identity used while a broker ticket is still unknown.
    /// \note A non-zero value is required when `ticket` is zero in a descriptor.
    std::uint64_t correlation_id = 0;
    /// \brief Causal transition expected from this predicate after dispatch.
    ReconciliationTransition expected_transition =
        ReconciliationTransition::unspecified;
};

/// \brief Builds a durable causal predicate, including unknown-ticket identity.
/// \param kind Presence/absence domain asserted by the transition.
/// \param ticket Known broker ticket, or empty before `order_send` assigns one.
/// \param baseline_present Whether the identity was present before dispatch.
/// \param transition Expected causal transition.
/// \param correlation_id Stable client-side correlation while the ticket is unknown.
/// \param history_window Optional history coverage window.
/// \return Predicate suitable for a durable reconciliation descriptor.
inline ReconciliationPredicate expect_reconciliation_transition(
    ReconciliationPredicateKind kind, std::optional<std::uint64_t> ticket,
    bool baseline_present, ReconciliationTransition transition,
    std::uint64_t correlation_id = 0,
    std::optional<ObservationWindow> history_window = std::nullopt) {
    return {kind, ticket.value_or(0), std::move(history_window), baseline_present,
            correlation_id, transition};
}

/// \brief Annotates a ticket predicate with its pre-dispatch causal state.
/// \param predicate Existing domain predicate.
/// \param baseline_present Whether its asserted identity existed before dispatch.
/// \param correlation_id Stable client-side identity for an unknown ticket.
/// \return The same predicate with a matching expected transition.
inline ReconciliationPredicate with_reconciliation_transition(
    ReconciliationPredicate predicate, bool baseline_present,
    std::uint64_t correlation_id = 0) {
    const bool absence =
        predicate.kind == ReconciliationPredicateKind::active_order_absent ||
        predicate.kind == ReconciliationPredicateKind::position_absent ||
        predicate.kind == ReconciliationPredicateKind::history_order_absent ||
        predicate.kind == ReconciliationPredicateKind::history_deal_absent;
    predicate.baseline_present = baseline_present;
    predicate.correlation_id = correlation_id;
    predicate.expected_transition =
        absence ? ReconciliationTransition::present_to_absent
                : ReconciliationTransition::absent_to_present;
    return predicate;
}

/// \brief Requires a post-baseline active order ticket.
/// \param ticket MT5 active order ticket.
/// \return Presence predicate.
inline ReconciliationPredicate require_active_order(std::uint64_t ticket) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::active_order_present, ticket, std::nullopt}, false);
}

/// \brief Requires a post-baseline active order ticket to be absent.
/// \param ticket MT5 active order ticket.
/// \return Absence predicate.
inline ReconciliationPredicate require_active_order_absent(std::uint64_t ticket) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::active_order_absent, ticket, std::nullopt}, true);
}

/// \brief Requires a post-baseline position ticket.
/// \param ticket MT5 position ticket.
/// \return Presence predicate.
inline ReconciliationPredicate require_position(std::uint64_t ticket) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::position_present, ticket, std::nullopt}, false);
}

/// \brief Requires a post-baseline position ticket to be absent.
/// \param ticket MT5 position ticket.
/// \return Absence predicate.
inline ReconciliationPredicate require_position_absent(std::uint64_t ticket) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::position_absent, ticket, std::nullopt}, true);
}

/// \brief Requires a history order ticket, optionally within a time window.
/// \param ticket MT5 history order ticket.
/// \param window Optional inclusive completion-time window constraining the match.
/// \return Presence predicate.
inline ReconciliationPredicate require_history_order(
    std::uint64_t ticket, std::optional<ObservationWindow> window = std::nullopt) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::history_order_present, ticket, window}, false);
}

/// \brief Requires a history order ticket to be absent in a covered window.
/// \param ticket MT5 history order ticket.
/// \param window Inclusive completion-time window.
/// \return Absence predicate.
inline ReconciliationPredicate require_history_order_absent(
    std::uint64_t ticket, ObservationWindow window) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::history_order_absent, ticket, window}, true);
}

/// \brief Requires a history deal ticket, optionally within a time window.
/// \param ticket MT5 history deal ticket.
/// \param window Optional inclusive deal-time window constraining the match.
/// \return Presence predicate.
inline ReconciliationPredicate require_history_deal(
    std::uint64_t ticket, std::optional<ObservationWindow> window = std::nullopt) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::history_deal_present, ticket, window}, false);
}

/// \brief Requires a history deal ticket to be absent in a covered window.
/// \param ticket MT5 history deal ticket.
/// \param window Inclusive deal-time window.
/// \return Absence predicate.
inline ReconciliationPredicate require_history_deal_absent(
    std::uint64_t ticket, ObservationWindow window) {
    return with_reconciliation_transition(
        {ReconciliationPredicateKind::history_deal_absent, ticket, window}, true);
}

/// \struct ReconciliationRequest
/// \brief Groups a baseline and explicit observation predicates.
struct ReconciliationRequest {
    std::optional<ReconciliationBaseline> baseline; ///< Capture before the operation.
    std::vector<ReconciliationPredicate> predicates; ///< Assertions to evaluate.
    bool trade_event_gap = false; ///< Event hints were incomplete and need snapshots.
    bool deadline_expired = false; ///< The caller's bounded observation deadline elapsed.
};

/// \struct ReconciliationResult
/// \brief Reports deterministic predicate evaluation state.
struct ReconciliationResult {
    ReconciliationOutcome outcome = ReconciliationOutcome::pending;
    ReconciliationReason reason = ReconciliationReason::none;
    std::uint64_t evaluated_revision = 0; ///< Graph revision used for this result.
    std::size_t predicate_count = 0; ///< Number of requested predicates.
    std::size_t satisfied_predicates = 0; ///< Predicates currently satisfied.
    std::size_t pending_predicates = 0; ///< Predicates lacking fresh evidence.
    std::size_t missing_predicates = 0; ///< Predicates not currently satisfied.
    std::size_t contradictory_predicates = 0; ///< Predicates with unusable or conflicting evidence.

    /// \brief Tests whether the result is settled by the current evidence.
    /// \return True only for confirmed, account-mismatch, or ambiguous results.
    bool resolved() const {
        return outcome == ReconciliationOutcome::confirmed ||
               outcome == ReconciliationOutcome::account_mismatch ||
               outcome == ReconciliationOutcome::ambiguous;
    }
};

/// \class ReconciliationEngine
/// \brief Evaluates observation predicates without runtime calls or side effects.
class ReconciliationEngine {
public:
    /// \brief Evaluates one request against the current graph evidence.
    /// \param graph Observation graph owned by the caller.
    /// \param request Baseline and explicit predicates.
    /// \return Deterministic observation-only result.
    static ReconciliationResult evaluate(const ObservationGraph &graph,
                                         const ReconciliationRequest &request) {
        ReconciliationResult result;
        result.evaluated_revision = graph.revision();
        result.predicate_count = request.predicates.size();

        if (!request.baseline || !request.baseline->valid() || request.predicates.empty() ||
            request.baseline->graph_revision() > graph.revision())
            return invalid(result);
        const auto &baseline = *request.baseline;
        for (const auto &predicate : request.predicates) {
            if (!valid_predicate(predicate))
                return invalid(result);
        }
        if (!graph.bound()) {
            result.reason = ReconciliationReason::waiting_for_observation;
            result.pending_predicates = result.predicate_count;
            return result;
        }
        if (graph.account_key() != baseline.account()) {
            result.outcome = ReconciliationOutcome::account_mismatch;
            result.reason = ReconciliationReason::account_mismatch;
            return result;
        }
        if (graph.instance_id() != baseline.graph_instance_id()) {
            result.outcome = ReconciliationOutcome::ambiguous;
            result.reason = ReconciliationReason::graph_mismatch;
            return result;
        }

        const auto active_orders = graph.active_orders();
        const auto positions = graph.positions();
        const auto history_orders = graph.history_orders();
        const auto history_deals = graph.history_deals();
        bool has_pending = false;
        bool has_missing = false;
        bool has_contradiction = false;
        bool has_unbound_identity = false;

        for (const auto &predicate : request.predicates) {
            // A broker-assigned identity is legitimately unknown before the
            // transport result arrives.  Keep that durable expectation
            // pending; an unknown ticket must never satisfy an absence
            // predicate or be treated as a confirmed presence.
            if (predicate.ticket == 0) {
                ++result.pending_predicates;
                has_pending = true;
                has_unbound_identity = true;
                continue;
            }
            switch (predicate.kind) {
            case ReconciliationPredicateKind::active_order_present:
            case ReconciliationPredicateKind::active_order_absent: {
                if (graph.domain_revision(ObservationDomain::active_orders) <=
                    baseline.active_orders_revision()) {
                    ++result.pending_predicates;
                    has_pending = true;
                    continue;
                }
                const bool found = contains_ticket(active_orders, predicate.ticket);
                const bool must_be_present =
                    predicate.kind == ReconciliationPredicateKind::active_order_present;
                if (found == must_be_present) {
                    ++result.satisfied_predicates;
                } else if (must_be_present) {
                    ++result.missing_predicates;
                    has_missing = true;
                } else {
                    ++result.missing_predicates;
                    has_missing = true;
                }
                continue;
            }
            case ReconciliationPredicateKind::position_present:
            case ReconciliationPredicateKind::position_absent: {
                if (graph.domain_revision(ObservationDomain::positions) <=
                    baseline.positions_revision()) {
                    ++result.pending_predicates;
                    has_pending = true;
                    continue;
                }
                const bool found = contains_ticket(positions, predicate.ticket);
                const bool must_be_present =
                    predicate.kind == ReconciliationPredicateKind::position_present;
                if (found == must_be_present) {
                    ++result.satisfied_predicates;
                } else if (must_be_present) {
                    ++result.missing_predicates;
                    has_missing = true;
                } else {
                    ++result.missing_predicates;
                    has_missing = true;
                }
                continue;
            }
            case ReconciliationPredicateKind::history_order_present:
            case ReconciliationPredicateKind::history_order_absent: {
                const auto *record = find_ticket(history_orders, predicate.ticket);
                const bool fresh_record =
                    graph.history_order_evidence_revision(predicate.ticket) >
                    baseline.history_orders_revision();
                const bool record_time_known =
                    record && has_field(record->known_fields, MT5BRIDGE_ORDER_KNOWN_TIME_DONE);
                if (predicate.kind == ReconciliationPredicateKind::history_order_present) {
                    const bool in_requested_window =
                        !predicate.history_window ||
                        (record_time_known &&
                         in_window(record->time_done_msc, *predicate.history_window));
                    if (fresh_record && record && in_requested_window) {
                        ++result.satisfied_predicates;
                    } else if (fresh_record && record && predicate.history_window &&
                               !record_time_known) {
                        ++result.contradictory_predicates;
                        has_contradiction = true;
                    } else if (predicate.history_window &&
                               graph.history_orders_covered(*predicate.history_window,
                                                             baseline.history_orders_revision())) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.pending_predicates;
                        has_pending = true;
                    }
                } else {
                    const bool covered =
                        graph.history_orders_covered(*predicate.history_window,
                                                      baseline.history_orders_revision());
                    if (!covered) {
                        ++result.pending_predicates;
                        has_pending = true;
                    } else if (fresh_record && record && !record_time_known) {
                        ++result.contradictory_predicates;
                        has_contradiction = true;
                    } else if (fresh_record && record &&
                               in_window(record->time_done_msc, *predicate.history_window)) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.satisfied_predicates;
                    }
                }
                continue;
            }
            case ReconciliationPredicateKind::history_deal_present:
            case ReconciliationPredicateKind::history_deal_absent: {
                const auto *record = find_ticket(history_deals, predicate.ticket);
                const bool fresh_record =
                    graph.history_deal_evidence_revision(predicate.ticket) >
                    baseline.history_deals_revision();
                const bool record_time_known =
                    record && has_field(record->known_fields, MT5BRIDGE_DEAL_KNOWN_TIME);
                if (predicate.kind == ReconciliationPredicateKind::history_deal_present) {
                    const bool in_requested_window =
                        !predicate.history_window ||
                        (record_time_known &&
                         in_window(record->time_msc, *predicate.history_window));
                    if (fresh_record && record && in_requested_window) {
                        ++result.satisfied_predicates;
                    } else if (fresh_record && record && predicate.history_window &&
                               !record_time_known) {
                        ++result.contradictory_predicates;
                        has_contradiction = true;
                    } else if (predicate.history_window &&
                               graph.history_deals_covered(*predicate.history_window,
                                                            baseline.history_deals_revision())) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.pending_predicates;
                        has_pending = true;
                    }
                } else {
                    const bool covered =
                        graph.history_deals_covered(*predicate.history_window,
                                                     baseline.history_deals_revision());
                    if (!covered) {
                        ++result.pending_predicates;
                        has_pending = true;
                    } else if (fresh_record && record && !record_time_known) {
                        ++result.contradictory_predicates;
                        has_contradiction = true;
                    } else if (fresh_record && record &&
                               in_window(record->time_msc, *predicate.history_window)) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.satisfied_predicates;
                    }
                }
                continue;
            }
            }
        }

        if (has_contradiction) {
            result.outcome = ReconciliationOutcome::ambiguous;
            result.reason = ReconciliationReason::contradictory_evidence;
        } else if (has_unbound_identity) {
            result.outcome = ReconciliationOutcome::pending;
            result.reason = ReconciliationReason::unresolved_operation;
        } else if (has_pending) {
            result.outcome = request.trade_event_gap ? ReconciliationOutcome::trade_event_gap
                                                     : ReconciliationOutcome::pending;
            result.reason = request.trade_event_gap ? ReconciliationReason::trade_event_gap
                                                    : ReconciliationReason::waiting_for_observation;
        } else if (has_missing && request.deadline_expired) {
            result.outcome = ReconciliationOutcome::not_observed;
            result.reason = ReconciliationReason::evidence_missing;
        } else if (has_missing) {
            result.outcome = ReconciliationOutcome::pending;
            result.reason = ReconciliationReason::waiting_for_observation;
        } else {
            result.outcome = ReconciliationOutcome::confirmed;
            result.reason = ReconciliationReason::none;
        }
        return result;
    }

private:
    static ReconciliationResult invalid(ReconciliationResult result) {
        result.outcome = ReconciliationOutcome::ambiguous;
        result.reason = ReconciliationReason::invalid_request;
        return result;
    }

    static bool valid_predicate(const ReconciliationPredicate &predicate) {
        if (predicate.ticket == 0 && predicate.correlation_id == 0)
            return false;
        if (predicate.ticket == 0 &&
            (predicate.expected_transition == ReconciliationTransition::unspecified ||
             !predicate.baseline_present))
            return false;
        switch (predicate.kind) {
        case ReconciliationPredicateKind::active_order_present:
        case ReconciliationPredicateKind::active_order_absent:
        case ReconciliationPredicateKind::position_present:
        case ReconciliationPredicateKind::position_absent:
        case ReconciliationPredicateKind::history_order_present:
        case ReconciliationPredicateKind::history_order_absent:
        case ReconciliationPredicateKind::history_deal_present:
        case ReconciliationPredicateKind::history_deal_absent:
            break;
        default:
            return false;
        }
        const bool history =
            predicate.kind == ReconciliationPredicateKind::history_order_present ||
            predicate.kind == ReconciliationPredicateKind::history_order_absent ||
            predicate.kind == ReconciliationPredicateKind::history_deal_present ||
            predicate.kind == ReconciliationPredicateKind::history_deal_absent;
        const bool absence =
            predicate.kind == ReconciliationPredicateKind::active_order_absent ||
            predicate.kind == ReconciliationPredicateKind::position_absent ||
            predicate.kind == ReconciliationPredicateKind::history_order_absent ||
            predicate.kind == ReconciliationPredicateKind::history_deal_absent;
        if (predicate.ticket == 0 && absence)
            return false;
        if (!history && predicate.history_window)
            return false;
        if (history && predicate.history_window && !predicate.history_window->valid())
            return false;
        if (absence && history && !predicate.history_window)
            return false;
        if (predicate.baseline_present) {
            const bool expected_presence = !absence;
            const auto expected_transition =
                expected_presence ? ReconciliationTransition::absent_to_present
                                  : ReconciliationTransition::present_to_absent;
            if (predicate.expected_transition != expected_transition ||
                *predicate.baseline_present != !expected_presence)
                return false;
        }
        return true;
    }

    template <typename T>
    static bool contains_ticket(const std::vector<T> &values, std::uint64_t ticket) {
        return find_ticket(values, ticket) != nullptr;
    }

    template <typename T>
    static const T *find_ticket(const std::vector<T> &values, std::uint64_t ticket) {
        for (const auto &value : values) {
            if (value.ticket == ticket)
                return &value;
        }
        return nullptr;
    }

    static bool in_window(std::int64_t value, const ObservationWindow &window) {
        return value >= window.from_msc && value <= window.to_msc;
    }

    static bool has_field(std::uint64_t known_fields, std::uint64_t field) {
        return (known_fields & field) == field;
    }
};

} // namespace mt5bridge

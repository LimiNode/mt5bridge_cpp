#pragma once

/// \file reconciliation_engine.hpp
/// \brief Defines observation-only reconciliation predicates over the graph.

#include "reconciliation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

/// \struct ReconciliationBaseline
/// \brief Captures graph revisions before an operation or observation cycle.
struct ReconciliationBaseline {
    AccountKey account; ///< Account scope captured with the revisions.
    std::uint64_t graph_revision = 0; ///< Global graph revision at capture time.
    std::uint64_t active_orders_revision = 0; ///< Active-order domain revision.
    std::uint64_t positions_revision = 0; ///< Position domain revision.
    std::uint64_t history_orders_revision = 0; ///< History-order domain revision.
    std::uint64_t history_deals_revision = 0; ///< History-deal domain revision.
};

/// \brief Captures all current graph revisions for a later reconciliation.
/// \param graph Observation graph owned by the caller.
/// \return Immutable baseline snapshot; an unbound graph produces an invalid account.
inline ReconciliationBaseline capture_reconciliation_baseline(
    const ObservationGraph &graph) {
    ReconciliationBaseline baseline;
    baseline.account = graph.account_key();
    baseline.graph_revision = graph.revision();
    baseline.active_orders_revision =
        graph.domain_revision(ObservationDomain::active_orders);
    baseline.positions_revision = graph.domain_revision(ObservationDomain::positions);
    baseline.history_orders_revision =
        graph.domain_revision(ObservationDomain::history_orders);
    baseline.history_deals_revision =
        graph.domain_revision(ObservationDomain::history_deals);
    return baseline;
}

/// \struct ReconciliationPredicate
/// \brief Describes one explicit evidence assertion.
struct ReconciliationPredicate {
    ReconciliationPredicateKind kind = ReconciliationPredicateKind::active_order_present;
    std::uint64_t ticket = 0; ///< Primary MT5 ticket asserted by the predicate.
    /// \brief Optional time window for a history predicate.
    /// \note Required for history absence and optional for history presence.
    std::optional<ObservationWindow> history_window;
};

/// \brief Requires a post-baseline active order ticket.
/// \param ticket MT5 active order ticket.
/// \return Presence predicate.
inline ReconciliationPredicate require_active_order(std::uint64_t ticket) {
    return {ReconciliationPredicateKind::active_order_present, ticket, std::nullopt};
}

/// \brief Requires a post-baseline active order ticket to be absent.
/// \param ticket MT5 active order ticket.
/// \return Absence predicate.
inline ReconciliationPredicate require_active_order_absent(std::uint64_t ticket) {
    return {ReconciliationPredicateKind::active_order_absent, ticket, std::nullopt};
}

/// \brief Requires a post-baseline position ticket.
/// \param ticket MT5 position ticket.
/// \return Presence predicate.
inline ReconciliationPredicate require_position(std::uint64_t ticket) {
    return {ReconciliationPredicateKind::position_present, ticket, std::nullopt};
}

/// \brief Requires a post-baseline position ticket to be absent.
/// \param ticket MT5 position ticket.
/// \return Absence predicate.
inline ReconciliationPredicate require_position_absent(std::uint64_t ticket) {
    return {ReconciliationPredicateKind::position_absent, ticket, std::nullopt};
}

/// \brief Requires a history order ticket, optionally within a time window.
/// \param ticket MT5 history order ticket.
/// \param window Optional inclusive completion-time window constraining the match.
/// \return Presence predicate.
inline ReconciliationPredicate require_history_order(
    std::uint64_t ticket, std::optional<ObservationWindow> window = std::nullopt) {
    return {ReconciliationPredicateKind::history_order_present, ticket, window};
}

/// \brief Requires a history order ticket to be absent in a covered window.
/// \param ticket MT5 history order ticket.
/// \param window Inclusive completion-time window.
/// \return Absence predicate.
inline ReconciliationPredicate require_history_order_absent(
    std::uint64_t ticket, ObservationWindow window) {
    return {ReconciliationPredicateKind::history_order_absent, ticket, window};
}

/// \brief Requires a history deal ticket, optionally within a time window.
/// \param ticket MT5 history deal ticket.
/// \param window Optional inclusive deal-time window constraining the match.
/// \return Presence predicate.
inline ReconciliationPredicate require_history_deal(
    std::uint64_t ticket, std::optional<ObservationWindow> window = std::nullopt) {
    return {ReconciliationPredicateKind::history_deal_present, ticket, window};
}

/// \brief Requires a history deal ticket to be absent in a covered window.
/// \param ticket MT5 history deal ticket.
/// \param window Inclusive deal-time window.
/// \return Absence predicate.
inline ReconciliationPredicate require_history_deal_absent(
    std::uint64_t ticket, ObservationWindow window) {
    return {ReconciliationPredicateKind::history_deal_absent, ticket, window};
}

/// \struct ReconciliationRequest
/// \brief Groups a baseline and explicit observation predicates.
struct ReconciliationRequest {
    ReconciliationBaseline baseline; ///< Revisions captured before the operation.
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

        if (!request.baseline.account.valid() || request.predicates.empty() ||
            request.baseline.graph_revision > graph.revision() ||
            request.baseline.active_orders_revision > request.baseline.graph_revision ||
            request.baseline.positions_revision > request.baseline.graph_revision ||
            request.baseline.history_orders_revision > request.baseline.graph_revision ||
            request.baseline.history_deals_revision > request.baseline.graph_revision)
            return invalid(result);
        for (const auto &predicate : request.predicates) {
            if (!valid_predicate(predicate))
                return invalid(result);
        }
        if (!graph.bound()) {
            result.reason = ReconciliationReason::waiting_for_observation;
            result.pending_predicates = result.predicate_count;
            return result;
        }
        if (graph.account_key() != request.baseline.account) {
            result.outcome = ReconciliationOutcome::account_mismatch;
            result.reason = ReconciliationReason::account_mismatch;
            return result;
        }

        const auto active_orders = graph.active_orders();
        const auto positions = graph.positions();
        const auto history_orders = graph.history_orders();
        const auto history_deals = graph.history_deals();
        bool has_pending = false;
        bool has_missing = false;
        bool has_contradiction = false;

        for (const auto &predicate : request.predicates) {
            switch (predicate.kind) {
            case ReconciliationPredicateKind::active_order_present:
            case ReconciliationPredicateKind::active_order_absent: {
                if (graph.domain_revision(ObservationDomain::active_orders) <=
                    request.baseline.active_orders_revision) {
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
                    request.baseline.positions_revision) {
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
                    request.baseline.history_orders_revision;
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
                                                            request.baseline
                                                                .history_orders_revision)) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.pending_predicates;
                        has_pending = true;
                    }
                } else {
                    const bool covered =
                        graph.history_orders_covered(*predicate.history_window,
                                                     request.baseline.history_orders_revision);
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
                    request.baseline.history_deals_revision;
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
                                                           request.baseline
                                                               .history_deals_revision)) {
                        ++result.missing_predicates;
                        has_missing = true;
                    } else {
                        ++result.pending_predicates;
                        has_pending = true;
                    }
                } else {
                    const bool covered =
                        graph.history_deals_covered(*predicate.history_window,
                                                    request.baseline.history_deals_revision);
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
        if (predicate.ticket == 0)
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
        if (!history && predicate.history_window)
            return false;
        if (history && predicate.history_window && !predicate.history_window->valid())
            return false;
        if (absence && history && !predicate.history_window)
            return false;
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

#pragma once

/// \file reconciliation/coordinator.hpp
/// \brief Defines synchronous observation collection and pre-dispatch checks.

#include <mt5bridge/client.hpp>
#include <mt5bridge/reconciliation/engine.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

/// \struct ObservationCollectionRequest
/// \brief Selects account-wide domains for one authoritative collection.
struct ObservationCollectionRequest {
    bool observe_active_orders = true; ///< Collect the complete active-order snapshot.
    bool observe_positions = true; ///< Collect the complete position snapshot.
    std::optional<ObservationWindow> history_orders_window; ///< Optional complete order window.
    std::optional<ObservationWindow> history_deals_window; ///< Optional complete deal window.

    /// \brief Tests whether requested history windows are valid.
    /// \return True when at least one domain is selected and every supplied
    /// inclusive window is ordered and non-negative.
    bool valid() const {
        const bool has_domain = observe_active_orders || observe_positions ||
                                history_orders_window.has_value() ||
                                history_deals_window.has_value();
        return has_domain && (!history_orders_window || history_orders_window->valid()) &&
               (!history_deals_window || history_deals_window->valid());
    }
};

/// \class ObservationProvider
/// \brief Supplies one fully assembled observation batch to a coordinator.
class ObservationProvider {
public:
    virtual ~ObservationProvider() = default;

    /// \brief Collects a batch without modifying the coordinator graph.
    /// \param request Account-wide domains and bounded history windows to collect.
    /// \return Complete batch ready for one atomic graph apply.
    /// \throws std::exception When the underlying observation source fails.
    virtual ObservationBatch collect(const ObservationCollectionRequest &request) = 0;
};

/// \class ClientObservationProvider
/// \brief Adapts typed `Client` observations to the coordinator provider seam.
class ClientObservationProvider final : public ObservationProvider {
public:
    /// \brief Binds the provider to a caller-owned client.
    /// \param client Initialized client used for typed observation calls.
    explicit ClientObservationProvider(Client &client) : client_(client) {}

    /// \brief Collects account-wide active and optional bounded history evidence.
    /// \param request Domains and windows to query.
    /// \return Batch whose account identity came from `Client::account_info()`.
    ObservationBatch collect(const ObservationCollectionRequest &request) override {
        if (!request.valid())
            throw std::invalid_argument("invalid observation collection window");

        ObservationBatch batch;
        const auto account_before = make_account_key(client_.account_info());
        if (!account_before.valid())
            throw std::runtime_error("account identity unavailable before observation");
        batch.account = account_before;
        if (request.observe_active_orders) {
            batch.observed_domains = batch.observed_domains |
                                     ObservationDomain::active_orders;
            batch.active_orders = client_.orders();
        }
        if (request.observe_positions) {
            batch.observed_domains = batch.observed_domains |
                                     ObservationDomain::positions;
            batch.positions = client_.positions();
        }
        if (request.history_orders_window) {
            batch.observed_domains = batch.observed_domains |
                                     ObservationDomain::history_orders;
            batch.history_orders_window = request.history_orders_window;
            Mt5HistoryOrdersRequest query{};
            query.from_msc = request.history_orders_window->from_msc;
            query.to_msc = request.history_orders_window->to_msc;
            batch.history_orders = client_.history_orders(query);
        }
        if (request.history_deals_window) {
            batch.observed_domains = batch.observed_domains |
                                     ObservationDomain::history_deals;
            batch.history_deals_window = request.history_deals_window;
            Mt5HistoryDealsRequest query{};
            query.from_msc = request.history_deals_window->from_msc;
            query.to_msc = request.history_deals_window->to_msc;
            batch.history_deals = client_.history_deals(query);
        }
        const auto account_after = make_account_key(client_.account_info());
        if (!account_after.valid())
            throw std::runtime_error("account identity unavailable after observation");
        if (account_after != account_before)
            throw std::runtime_error("account changed during observation collection");
        return batch;
    }

private:
    Client &client_;
};

/// \class ObservationSample
/// \brief Provenance-bearing batch accepted by one coordinator graph revision.
///
/// The constructor is private so application code cannot manufacture a sample
/// that was not admitted by `ObservationCoordinator::refresh()`.
class ObservationSample {
public:
    ObservationSample(const ObservationSample &) = default;
    ObservationSample &operator=(const ObservationSample &) = default;
    ObservationSample(ObservationSample &&) = default;
    ObservationSample &operator=(ObservationSample &&) = default;

    /// \brief Returns the accepted observation payload.
    /// \return Immutable batch collected for this sample.
    const ObservationBatch &batch() const { return batch_; }

    /// \brief Returns the owning graph's process-local identity.
    /// \return Graph instance identity captured at admission.
    std::uint64_t graph_instance_id() const { return graph_instance_id_; }

    /// \brief Returns the graph revision that accepted this batch.
    /// \return Strictly increasing owner-loop revision.
    std::uint64_t graph_revision() const { return graph_revision_; }

private:
    static ObservationSample create(ObservationBatch batch,
                                    std::uint64_t graph_instance_id,
                                    std::uint64_t graph_revision) {
        return ObservationSample(std::move(batch), graph_instance_id, graph_revision);
    }

    ObservationSample(ObservationBatch batch, std::uint64_t graph_instance_id,
                      std::uint64_t graph_revision)
        : batch_(std::move(batch)),
          graph_instance_id_(graph_instance_id),
          graph_revision_(graph_revision) {}

    ObservationBatch batch_;
    std::uint64_t graph_instance_id_ = 0;
    std::uint64_t graph_revision_ = 0;

    friend class ObservationCoordinator;
};

/// \struct ObservationRefreshResult
/// \brief Reports graph admission and its optional accepted sample.
struct ObservationRefreshResult {
    ObservationApplyResult apply; ///< Graph admission status and revision.
    std::optional<ObservationSample> sample; ///< Present only after acceptance.
};

/// \class ObservationCoordinator
/// \brief Owns one synchronous graph update loop without runtime side effects.
class ObservationCoordinator {
public:
    /// \brief Binds a provider and optionally fixes the graph account scope.
    /// \param provider Observation source owned by the caller.
    /// \param account Optional account scope known before the first collection.
    explicit ObservationCoordinator(ObservationProvider &provider,
                                    AccountKey account = {})
        : provider_(provider), graph_(std::move(account)) {}

    ObservationCoordinator(const ObservationCoordinator &) = delete;
    ObservationCoordinator &operator=(const ObservationCoordinator &) = delete;
    ObservationCoordinator(ObservationCoordinator &&) = delete;
    ObservationCoordinator &operator=(ObservationCoordinator &&) = delete;

    /// \brief Collects one provider batch and atomically applies it to the graph.
    /// \param request Account-wide domains and bounded history windows to collect.
    /// \return Admission plus a provenance-bearing sample after success;
    /// provider failures propagate as exceptions.
    ObservationRefreshResult refresh(const ObservationCollectionRequest &request) {
        if (!request.valid())
            return {{ObservationApplyStatus::invalid_evidence, graph_.revision()}, std::nullopt};
        auto batch = provider_.collect(request);
        if (!matches_request(request, batch))
            return {{ObservationApplyStatus::invalid_evidence, graph_.revision()}, std::nullopt};
        const auto admission = graph_.apply(batch);
        ObservationRefreshResult result{admission, std::nullopt};
        if (admission.accepted())
            result.sample.emplace(ObservationSample::create(
                std::move(batch), graph_.instance_id(), admission.revision));
        return result;
    }

    /// \brief Captures a baseline owned by this coordinator's graph.
    /// \return Baseline carrying account and process-local graph provenance.
    ReconciliationBaseline capture_baseline() const {
        return capture_reconciliation_baseline(graph_);
    }

    /// \brief Evaluates predicates against the coordinator graph.
    /// \param request Baseline and explicit reconciliation predicates.
    /// \return Pure observation result with no provider call or side effect.
    ReconciliationResult evaluate(const ReconciliationRequest &request) const {
        return ReconciliationEngine::evaluate(graph_, request);
    }

    /// \brief Returns the graph for read-only inspection and link queries.
    /// \return Const graph owned by this coordinator.
    const ObservationGraph &graph() const { return graph_; }

private:
    static bool same_window(const std::optional<ObservationWindow> &left,
                            const std::optional<ObservationWindow> &right) {
        if (left.has_value() != right.has_value())
            return false;
        return !left || (left->from_msc == right->from_msc &&
                         left->to_msc == right->to_msc);
    }

    static bool matches_request(const ObservationCollectionRequest &request,
                                const ObservationBatch &batch) {
        ObservationDomain expected_domains = ObservationDomain::none;
        if (request.observe_active_orders)
            expected_domains = expected_domains | ObservationDomain::active_orders;
        if (request.observe_positions)
            expected_domains = expected_domains | ObservationDomain::positions;
        if (request.history_orders_window)
            expected_domains = expected_domains | ObservationDomain::history_orders;
        if (request.history_deals_window)
            expected_domains = expected_domains | ObservationDomain::history_deals;

        return batch.observed_domains == expected_domains &&
               (request.observe_active_orders || batch.active_orders.empty()) &&
               (request.observe_positions || batch.positions.empty()) &&
               ((!request.history_orders_window && batch.history_orders.empty()) ||
                request.history_orders_window.has_value()) &&
               ((!request.history_deals_window && batch.history_deals.empty()) ||
                request.history_deals_window.has_value()) &&
               same_window(request.history_orders_window, batch.history_orders_window) &&
               same_window(request.history_deals_window, batch.history_deals_window);
    }

    ObservationProvider &provider_;
    ObservationGraph graph_;
};

/// \enum DispatchConsistencyState
/// \brief Describes whether requested observation evidence permits a future dispatch.
enum class DispatchConsistencyState {
    ready,                  ///< Required post-baseline evidence is fresh.
    waiting_for_active_orders, ///< Active orders need an authoritative refresh.
    waiting_for_positions,  ///< Positions need an authoritative refresh.
    waiting_for_history,    ///< Requested history coverage is incomplete.
    unresolved_operation,   ///< A prior operation remains unresolved.
    account_mismatch,       ///< Graph and baseline account identities differ.
    graph_mismatch,         ///< Baseline belongs to another graph instance.
    event_gap,              ///< Event hints are incomplete.
    invalid_request,        ///< Baseline or collection requirements are invalid.
};

/// \struct DispatchConsistencyRequest
/// \brief States the evidence required before a caller may dispatch.
struct DispatchConsistencyRequest {
    std::optional<ReconciliationBaseline> baseline; ///< Captured graph checkpoint.
    bool require_active_orders = true; ///< Require a post-baseline active snapshot.
    bool require_positions = true; ///< Require a post-baseline position snapshot.
    std::optional<ObservationWindow> history_orders_window; ///< Optional required coverage.
    std::optional<ObservationWindow> history_deals_window; ///< Optional required coverage.
    bool unresolved_operation = false; ///< Caller has an unresolved prior operation.
    bool event_gap = false; ///< Caller observed an incomplete hint stream.

    /// \brief Tests whether all requested history windows are valid.
    /// \return True when at least one evidence requirement is present and
    /// supplied windows are ordered and non-negative.
    bool valid() const {
        const bool has_requirement = require_active_orders || require_positions ||
                                     history_orders_window.has_value() ||
                                     history_deals_window.has_value();
        return has_requirement &&
               (!history_orders_window || history_orders_window->valid()) &&
               (!history_deals_window || history_deals_window->valid());
    }
};

/// \struct DispatchConsistencyResult
/// \brief Reports an evidence-based observation-readiness decision.
struct DispatchConsistencyResult {
    DispatchConsistencyState state = DispatchConsistencyState::invalid_request;
    ReconciliationReason reason = ReconciliationReason::invalid_request;
    std::uint64_t evaluated_revision = 0; ///< Graph revision used for the decision.

    /// \brief Tests whether the caller may proceed to a future dispatch layer.
    /// \return True only for fresh requested evidence; this does not prove an
    /// atomic cross-domain MT5 snapshot or submit an order.
    bool ready() const { return state == DispatchConsistencyState::ready; }
};

/// \class DispatchConsistencyGate
/// \brief Evaluates observation readiness before any future side effect.
///
/// This gate checks graph provenance, requested freshness, coverage, and
/// caller-owned blockers. It does not prove that sequential MT5 queries came
/// from one atomic terminal snapshot; a later environment-consistency policy
/// must own that stronger claim.
class DispatchConsistencyGate {
public:
    /// \brief Evaluates the gate without querying the runtime or changing state.
    /// \param graph Observation graph owned by the caller's coordinator loop.
    /// \param request Baseline, required domains, and unresolved hints.
    /// \return Evidence-based gate state; `ready` does not submit anything.
    static DispatchConsistencyResult evaluate(
        const ObservationGraph &graph, const DispatchConsistencyRequest &request) {
        DispatchConsistencyResult result;
        result.evaluated_revision = graph.revision();
        if (!request.valid() || !request.baseline || !request.baseline->valid() ||
            request.baseline->graph_revision() > graph.revision())
            return result;

        const auto &baseline = *request.baseline;
        if (!graph.bound() || graph.account_key() != baseline.account()) {
            result.state = DispatchConsistencyState::account_mismatch;
            result.reason = ReconciliationReason::account_mismatch;
            return result;
        }
        if (graph.instance_id() != baseline.graph_instance_id()) {
            result.state = DispatchConsistencyState::graph_mismatch;
            result.reason = ReconciliationReason::graph_mismatch;
            return result;
        }
        if (request.event_gap) {
            result.state = DispatchConsistencyState::event_gap;
            result.reason = ReconciliationReason::trade_event_gap;
            return result;
        }
        if (request.unresolved_operation) {
            result.state = DispatchConsistencyState::unresolved_operation;
            result.reason = ReconciliationReason::unresolved_operation;
            return result;
        }
        if (request.require_active_orders &&
            graph.domain_revision(ObservationDomain::active_orders) <=
                baseline.active_orders_revision()) {
            result.state = DispatchConsistencyState::waiting_for_active_orders;
            result.reason = ReconciliationReason::waiting_for_observation;
            return result;
        }
        if (request.require_positions &&
            graph.domain_revision(ObservationDomain::positions) <=
                baseline.positions_revision()) {
            result.state = DispatchConsistencyState::waiting_for_positions;
            result.reason = ReconciliationReason::waiting_for_observation;
            return result;
        }
        if ((request.history_orders_window &&
             !graph.history_orders_covered(*request.history_orders_window,
                                           baseline.history_orders_revision())) ||
            (request.history_deals_window &&
             !graph.history_deals_covered(*request.history_deals_window,
                                          baseline.history_deals_revision()))) {
            result.state = DispatchConsistencyState::waiting_for_history;
            result.reason = ReconciliationReason::waiting_for_observation;
            return result;
        }
        result.state = DispatchConsistencyState::ready;
        result.reason = ReconciliationReason::none;
        return result;
    }
};

} // namespace mt5bridge

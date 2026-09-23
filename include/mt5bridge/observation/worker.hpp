#pragma once

/// \file observation/worker.hpp
/// \brief Defines the caller-driven bounded reconciliation worker.

#include <mt5bridge/observation/coordinator.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace mt5bridge {

/// \struct ReconciliationWorkerCycle
/// \brief Couples one authoritative refresh with its predicate result.
struct ReconciliationWorkerCycle {
    ObservationRefreshResult refresh; ///< Graph admission and accepted sample.
    ReconciliationResult reconciliation; ///< Evaluation after this refresh.
    std::uint64_t cycle = 0; ///< One-based caller-driven cycle number.

    /// \brief Tests whether this cycle reached a settled reconciliation state.
    /// \return True for confirmed, account-mismatch, or ambiguous outcomes.
    bool settled() const { return reconciliation.resolved(); }
};

/// \class ReconciliationWorker
/// \brief Runs one bounded, caller-driven reconciliation cycle at a time.
///
/// The worker owns no thread and performs no transport or trading side effect.
/// The owner loop chooses when to call `step()`, supplies event-gap/deadline
/// hints, and keeps unresolved operations alive after `not_observed`.
class ReconciliationWorker {
public:
    /// \brief Binds a worker to one coordinator and operation request.
    /// \param coordinator Coordinator that owns authoritative graph updates.
    /// \param collection_request Domains refreshed on every cycle.
    /// \param reconciliation_request Baseline and predicates for one operation.
    ReconciliationWorker(ObservationCoordinator &coordinator,
                         ObservationCollectionRequest collection_request,
                         ReconciliationRequest reconciliation_request)
        : coordinator_(coordinator),
          collection_request_(std::move(collection_request)),
          reconciliation_request_(std::move(reconciliation_request)) {}

    ReconciliationWorker(const ReconciliationWorker &) = delete;
    ReconciliationWorker &operator=(const ReconciliationWorker &) = delete;

    /// \brief Performs one authoritative refresh and evaluates the operation.
    /// \param trade_event_gap True when an event hint stream lost continuity.
    /// \param deadline_expired True when this bounded wait deadline elapsed.
    /// \return Refresh provenance and pure reconciliation result.
    /// \throws std::exception When the provider fails during collection.
    ReconciliationWorkerCycle step(bool trade_event_gap = false,
                                   bool deadline_expired = false) {
        if (last_cycle_ && last_cycle_->settled())
            return *last_cycle_;

        auto refresh = coordinator_.refresh(collection_request_);
        auto request = reconciliation_request_;
        request.trade_event_gap = request.trade_event_gap || trade_event_gap;
        request.deadline_expired = request.deadline_expired || deadline_expired;
        ReconciliationResult reconciliation;
        if (refresh.apply.accepted()) {
            reconciliation = coordinator_.evaluate(request);
        } else {
            reconciliation.evaluated_revision = coordinator_.graph().revision();
            reconciliation.predicate_count = request.predicates.size();
            if (refresh.apply.status == ObservationApplyStatus::account_mismatch) {
                reconciliation.outcome = ReconciliationOutcome::account_mismatch;
                reconciliation.reason = ReconciliationReason::account_mismatch;
            } else {
                reconciliation.outcome = ReconciliationOutcome::ambiguous;
                reconciliation.reason = ReconciliationReason::contradictory_evidence;
            }
        }
        ReconciliationWorkerCycle cycle{
            std::move(refresh), std::move(reconciliation), ++cycle_count_};
        last_cycle_ = cycle;
        return cycle;
    }

    /// \brief Returns the latest cycle without triggering another refresh.
    /// \return Last cycle, or empty before the first step.
    const std::optional<ReconciliationWorkerCycle> &last_cycle() const {
        return last_cycle_;
    }

    /// \brief Returns the number of provider refreshes performed.
    /// \return One-based cycle count, or zero before the first step.
    std::uint64_t cycle_count() const { return cycle_count_; }

private:
    ObservationCoordinator &coordinator_;
    ObservationCollectionRequest collection_request_;
    ReconciliationRequest reconciliation_request_;
    std::uint64_t cycle_count_ = 0;
    std::optional<ReconciliationWorkerCycle> last_cycle_;
};

} // namespace mt5bridge

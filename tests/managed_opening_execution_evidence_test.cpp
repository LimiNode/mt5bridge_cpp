/// \file managed_opening_execution_evidence_test.cpp
/// \brief Verifies immutable OPEN execution evidence and frontier contracts.

#include "managed_opening_execution_evidence.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace mt5bridge::dispatch {

// Test-only seam. Production has no producer or owner wiring in this slice.
class ManagedOpeningExecutionEvidenceProducer {
public:
    static std::optional<ManagedOpeningDealEvidence> seal(
        const ManagedOpeningExecutionRequest &request,
        const ManagedOpeningDealObservation &observation) {
        if (validate_managed_opening_observation(request, observation) !=
            ManagedOpeningExecutionValidationStatus::valid)
            return std::nullopt;
        return ManagedOpeningDealEvidence(request, observation);
    }

    static ManagedOpeningExecutionFrontier frontier(
        const AccountKey &account, const OperationKey &operation_key,
        std::uint64_t operation_revision, std::uint64_t requested,
        std::vector<ManagedOpeningDealEvidence> entries, bool terminal) {
        std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
            return left.deal_ticket() < right.deal_ticket();
        });
        return ManagedOpeningExecutionFrontier(
            account, operation_key, operation_revision, requested,
            std::move(entries), terminal);
    }
};

} // namespace mt5bridge::dispatch

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

AccountKey account() { return {"demo", 42}; }

OperationKey operation() { return {account(), 71, 9}; }

ManagedOpeningExecutionRequest request(std::uint64_t source_revision = 4) {
    return {account(), operation(), OperationKind::open, 3, 3001, 88001,
            source_revision, source_revision};
}

std::optional<ManagedOpeningQuantity> quantity(double volume) {
    return derive_managed_opening_quantity(
        volume, 0.01, ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000});
}

ManagedOpeningDealObservation observation(std::uint64_t deal_ticket,
                                          std::uint64_t source_revision,
                                          double volume = 0.05) {
    return {account(), operation(), 3, source_revision, 99, source_revision,
            1000, 2000, 3001, deal_ticket, 88001,
            kManagedOpeningRequiredDealFields,
            BrokerPositionDirection::buy, BrokerDealEntry::in, 1500,
            volume, 0.01, 1, quantity(volume)};
}

ManagedOpeningDealEvidence evidence(std::uint64_t deal_ticket,
                                    std::uint64_t source_revision,
                                    double volume = 0.05) {
    const auto candidate = observation(deal_ticket, source_revision, volume);
    const auto result = ManagedOpeningExecutionEvidenceProducer::seal(
        request(source_revision), candidate);
    require(result.has_value(), "valid source observation was rejected");
    return *result;
}

void accepts_complete_source_and_canonical_replay() {
    const auto source = observation(5001, 4);
    require(validate_managed_opening_observation(request(), source) ==
                ManagedOpeningExecutionValidationStatus::valid,
            "complete source observation was rejected");
    const auto first = evidence(5001, 4);
    const auto second = evidence(5001, 4);
    require(first.valid(), "sealed opening evidence is invalid");
    require(first == second, "canonical replay changed immutable evidence");
    require(first.canonical_bytes() == second.canonical_bytes(),
            "canonical evidence bytes are not deterministic");
    require(first.evidence_digest() != 0, "opening evidence digest is empty");
    require(classify_managed_opening_replay(first, second) ==
                ManagedOpeningExecutionReplayStatus::already_committed,
            "exact deal replay was not idempotent");
    static_assert(!std::is_default_constructible_v<ManagedOpeningDealEvidence>);
}

void rejects_missing_history_and_foreign_identity() {
    auto candidate = observation(5001, 4);
    candidate.history_window_to_msc = 1499;
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::missing_history,
            "deal outside history coverage was accepted");

    candidate = observation(5001, 4);
    candidate.account = {"other", 42};
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::account_mismatch,
            "foreign account evidence was accepted");

    candidate = observation(5001, 4);
    candidate.order_ticket = 3002;
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::order_mismatch,
            "foreign order evidence was accepted");

    candidate = observation(5001, 4);
    candidate.position_identifier = 88002;
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::position_mismatch,
            "foreign position evidence was accepted");

    candidate = observation(5001, 4);
    candidate.entry = BrokerDealEntry::inout;
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::foreign_or_invalid_deal,
            "INOUT evidence crossed the OPEN gate");

    candidate = observation(5001, 4);
    candidate.known_fields &= ~static_cast<std::uint64_t>(MT5BRIDGE_DEAL_KNOWN_TIME);
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::foreign_or_invalid_deal,
            "missing deal timestamp field was accepted");

    candidate = observation(5001, 4);
    candidate.source_revision = 5;
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::source_revision_mismatch,
            "source revision mismatch was accepted");
}

void rejects_inexact_or_missing_quantity() {
    auto candidate = observation(5001, 4, 0.015);
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::invalid_quantity,
            "fractional broker volume was accepted as exact evidence");

    candidate = observation(5001, 4);
    candidate.quantity.reset();
    require(validate_managed_opening_observation(request(), candidate) ==
                ManagedOpeningExecutionValidationStatus::invalid_quantity,
            "missing exact quantity was accepted");
}

void protects_duplicate_execution_and_conflicting_replay() {
    const auto first = evidence(5001, 4);
    const auto different_deal = evidence(5002, 5, 0.03);
    require(classify_managed_opening_replay(first, different_deal) ==
                ManagedOpeningExecutionReplayStatus::new_execution,
            "new deal identity was treated as a replay");

    const auto conflicting = evidence(5001, 5, 0.03);
    require(classify_managed_opening_replay(first, conflicting) ==
                ManagedOpeningExecutionReplayStatus::conflict,
            "same deal ticket with different facts was accepted");
}

void builds_partial_frontier_and_extends_without_rewrite() {
    const auto first = evidence(5001, 4, 0.05);
    const auto second = evidence(5002, 5, 0.03);
    const auto third = evidence(5003, 6, 0.02);
    auto frontier = ManagedOpeningExecutionEvidenceProducer::frontier(
        account(), operation(), 3, 10, {first}, false);
    require(frontier.valid() && !frontier.terminal() &&
                frontier.cumulative_managed_logical_units() == 5,
            "partial opening frontier is invalid");

    const auto extended = append_managed_opening_execution(frontier, second);
    require(extended.appended() && extended.frontier &&
                !extended.frontier->terminal() &&
                extended.frontier->cumulative_managed_logical_units() == 8,
            "partial frontier extension failed");
    require(frontier.entries().size() == 1 &&
                frontier.cumulative_managed_logical_units() == 5,
            "frontier extension rewrote historical state");

    const auto terminal = append_managed_opening_execution(*extended.frontier, third);
    require(terminal.appended() && terminal.frontier && terminal.frontier->terminal() &&
                terminal.frontier->cumulative_managed_logical_units() == 10,
            "terminal opening frontier was not reached");
    require(terminal.frontier->entries().size() == 3,
            "late fill did not extend the cumulative frontier");

    const auto duplicate = append_managed_opening_execution(*extended.frontier, second);
    require(duplicate.status == ManagedOpeningExecutionFrontierAppendStatus::duplicate,
            "duplicate deal was appended twice");
    const auto stale = append_managed_opening_execution(
        *extended.frontier, evidence(5004, 5, 0.01));
    require(stale.status == ManagedOpeningExecutionFrontierAppendStatus::source_revision_mismatch,
            "stale source revision extended the frontier");
}

void keeps_frontier_canonical_and_terminal_replay_safe() {
    const auto first = evidence(5001, 4, 0.05);
    const auto second = evidence(5002, 5, 0.03);
    const auto left = ManagedOpeningExecutionEvidenceProducer::frontier(
        account(), operation(), 3, 8, {first, second}, true);
    const auto right = ManagedOpeningExecutionEvidenceProducer::frontier(
        account(), operation(), 3, 8, {second, first}, true);
    require(left.valid() && right.valid() && left == right,
            "frontier canonicalization depends on source order");
    const auto late = append_managed_opening_execution(left, evidence(5003, 6, 0.01));
    require(late.status == ManagedOpeningExecutionFrontierAppendStatus::terminal_frontier,
            "terminal frontier accepted a late execution");
}

void rejects_frontier_mixing_and_overfill() {
    const auto first = evidence(5001, 4, 0.05);
    auto foreign_observation = observation(5004, 4, 0.01);
    foreign_observation.operation_key = OperationKey{account(), 71, 10};
    auto frontier = ManagedOpeningExecutionEvidenceProducer::frontier(
        account(), operation(), 3, 4, {first}, false);
    require(!frontier.valid(), "over-target partial frontier was accepted");

    const auto foreign_operation = ManagedOpeningExecutionEvidenceProducer::seal(
        ManagedOpeningExecutionRequest{account(), OperationKey{account(), 71, 10},
                                       OperationKind::open, 3, 3001, 88001, 4, 4},
        foreign_observation);
    require(foreign_operation.has_value(),
            "test foreign operation evidence could not be sealed");
    const auto valid_frontier = ManagedOpeningExecutionEvidenceProducer::frontier(
        account(), operation(), 3, 10, {first}, false);
    const auto result = append_managed_opening_execution(valid_frontier,
                                                         *foreign_operation);
    require(result.status == ManagedOpeningExecutionFrontierAppendStatus::operation_mismatch,
            "foreign operation was mixed into opening frontier");
}

} // namespace

int main() {
    try {
        accepts_complete_source_and_canonical_replay();
        rejects_missing_history_and_foreign_identity();
        rejects_inexact_or_missing_quantity();
        protects_duplicate_execution_and_conflicting_replay();
        builds_partial_frontier_and_extends_without_rewrite();
        keeps_frontier_canonical_and_terminal_replay_safe();
        rejects_frontier_mixing_and_overfill();
        std::cout << "managed opening execution evidence tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

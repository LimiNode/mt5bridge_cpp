#pragma once

/// \file dispatch/managed_deal_settlement_producer.hpp
/// \brief Defines the private authoritative producer for managed deal facts.

#include <mt5bridge/dispatch/journal_types.hpp>
#include <mt5bridge/dispatch/managed_deal_settlement.hpp>
#include <mt5bridge/reconciliation/graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge::dispatch {
namespace detail {

inline void settlement_digest_u64(std::uint64_t *value, std::uint64_t input) {
    *value ^= input;
    *value *= 1099511628211ULL;
}

inline std::optional<std::uint64_t> settlement_anchor_ticket(
    const OperationRecord &operation) {
    if (!operation.reconciliation_descriptor)
        return std::nullopt;
    std::size_t count = 0;
    std::uint64_t ticket = 0;
    for (const auto &predicate : operation.reconciliation_descriptor->predicates) {
        if (predicate.kind != ReconciliationPredicateKind::history_deal_present)
            continue;
        if (++count != 1)
            return std::nullopt;
        ticket = predicate.ticket;
        if (ticket == 0) {
            if (predicate.correlation_id == 0)
                return std::nullopt;
            std::size_t matches = 0;
            for (const auto &binding : operation.reconciliation_bindings) {
                if (binding.correlation_id == predicate.correlation_id) {
                    if (++matches != 1 || !binding.valid())
                        return std::nullopt;
                    ticket = binding.broker_ticket;
                }
            }
            if (matches != 1)
                return std::nullopt;
        }
    }
    return count == 1 && ticket != 0 ? std::optional<std::uint64_t>(ticket)
                                     : std::nullopt;
}

inline std::optional<Mt5DealSnapshot> settlement_deal(
    const ObservationGraph &graph, std::uint64_t ticket) {
    for (const auto &deal : graph.history_deals()) {
        if (deal.ticket == ticket)
            return deal;
    }
    return std::nullopt;
}

} // namespace detail

/// \brief Derives one immutable per-deal fact from owner-supplied logical delta.
///
/// The logical-unit argument is produced by the private managed owner state
/// transition. This helper independently checks that the target deal is fresh,
/// complete, account-consistent, and tied to the operation's durable history
/// anchor; it never converts broker volume into logical units.
inline std::optional<ManagedDealSettlement> derive_managed_deal_settlement(
    const OperationRecord &operation, const ReconciliationBaseline &baseline,
    const ObservationGraph &graph, std::uint64_t deal_ticket,
    std::uint64_t managed_logical_units,
    const std::vector<ManagedDealSettlement> &prior_facts = {}) {
    if (!operation.valid() || operation.operation_kind != OperationKind::close ||
        (operation.operation_state != OperationState::partially_filled &&
         operation.operation_state != OperationState::filled) ||
        !operation.reconciliation_descriptor || managed_logical_units == 0 ||
        operation.settled_volume == 0 || managed_logical_units > operation.settled_volume ||
        operation.revision == 0 || !baseline.valid() || !graph.bound() ||
        graph.account_key() != operation.key.account ||
        baseline.account() != operation.key.account ||
        baseline.graph_instance_id() != graph.instance_id() ||
        operation.reconciliation_descriptor->baseline.graph_instance_id() !=
            baseline.graph_instance_id() ||
        operation.reconciliation_descriptor->baseline.graph_revision() !=
            baseline.graph_revision() ||
        graph.revision() <= baseline.graph_revision() ||
        graph.domain_revision(ObservationDomain::history_deals) <=
            baseline.history_deals_revision())
        return std::nullopt;

    const auto anchor_ticket = detail::settlement_anchor_ticket(operation);
    if (!anchor_ticket || !operation.reconciliation_descriptor)
        return std::nullopt;

    const ReconciliationPredicate *anchor_predicate = nullptr;
    for (const auto &predicate : operation.reconciliation_descriptor->predicates) {
        if (predicate.kind == ReconciliationPredicateKind::history_deal_present) {
            anchor_predicate = &predicate;
            break;
        }
    }
    if (!anchor_predicate || !anchor_predicate->history_window ||
        !anchor_predicate->history_window->valid() ||
        !graph.history_deals_covered(*anchor_predicate->history_window,
                                     baseline.history_deals_revision()))
        return std::nullopt;

    const auto anchor = detail::settlement_deal(graph, *anchor_ticket);
    const auto deal = detail::settlement_deal(graph, deal_ticket);
    constexpr std::uint64_t kRequiredDealFields =
        MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
        MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_ENTRY |
        MT5BRIDGE_DEAL_KNOWN_VOLUME | MT5BRIDGE_DEAL_KNOWN_TIME;
    if (!anchor || !deal ||
        (anchor->known_fields & kRequiredDealFields) != kRequiredDealFields ||
        (deal->known_fields & kRequiredDealFields) != kRequiredDealFields ||
        anchor->order_ticket == 0 || anchor->position_id == 0 ||
        deal->order_ticket != anchor->order_ticket ||
        deal->position_id != anchor->position_id ||
        graph.history_deal_evidence_revision(deal_ticket) <=
            baseline.history_deals_revision() ||
        deal->time_msc < anchor_predicate->history_window->from_msc ||
        deal->time_msc > anchor_predicate->history_window->to_msc ||
        !std::isfinite(deal->volume) || deal->volume <= 0.0 ||
        (deal->entry != 1U && deal->entry != 3U))
        return std::nullopt;

    const auto current_history_revision =
        graph.domain_revision(ObservationDomain::history_deals);
    if (graph.history_deal_evidence_revision(deal_ticket) !=
        current_history_revision)
        return std::nullopt;

    for (std::size_t index = 0; index != prior_facts.size(); ++index) {
        const auto &prior = prior_facts[index];
        if (!prior.valid() || prior.key.operation_key != operation.key ||
            prior.key.account != operation.key.account ||
            prior.source_operation_revision > operation.revision ||
            prior.key.deal_ticket == deal_ticket)
            return std::nullopt;
        const auto prior_deal = detail::settlement_deal(graph, prior.key.deal_ticket);
        if (!prior_deal ||
            graph.history_deal_evidence_revision(prior.key.deal_ticket) !=
                current_history_revision ||
            (prior_deal->known_fields & kRequiredDealFields) != kRequiredDealFields ||
            prior_deal->order_ticket != anchor->order_ticket ||
            prior_deal->position_id != anchor->position_id ||
            prior_deal->time_msc < anchor_predicate->history_window->from_msc ||
            prior_deal->time_msc > anchor_predicate->history_window->to_msc ||
            !std::isfinite(prior_deal->volume) || prior_deal->volume <= 0.0 ||
            (prior_deal->entry != 1U && prior_deal->entry != 3U))
            return std::nullopt;
        for (std::size_t previous = 0; previous != index; ++previous) {
            if (prior_facts[previous].key.deal_ticket == prior.key.deal_ticket)
                return std::nullopt;
        }
    }

    std::size_t newly_observed_contributions = 0;
    for (const auto &candidate : graph.history_deals()) {
        if (candidate.order_ticket != anchor->order_ticket ||
            candidate.position_id != anchor->position_id ||
            candidate.time_msc < anchor_predicate->history_window->from_msc ||
            candidate.time_msc > anchor_predicate->history_window->to_msc ||
            graph.history_deal_evidence_revision(candidate.ticket) !=
                current_history_revision)
            continue;
        if ((candidate.known_fields & kRequiredDealFields) != kRequiredDealFields ||
            (candidate.entry != 1U && candidate.entry != 3U))
            return std::nullopt;
        const auto is_prior_fact = std::any_of(
            prior_facts.begin(), prior_facts.end(),
            [&candidate](const auto &prior) {
                return prior.key.deal_ticket == candidate.ticket;
            });
        if (is_prior_fact)
            continue;
        ++newly_observed_contributions;
        if (candidate.ticket != deal_ticket)
            return std::nullopt;
    }
    if (newly_observed_contributions != 1)
        return std::nullopt;

    std::uint64_t digest = 1469598103934665603ULL;
    detail::settlement_digest_u64(&digest, operation.key.trade_id);
    detail::settlement_digest_u64(&digest, operation.key.operation_id);
    detail::settlement_digest_u64(&digest, operation.revision);
    detail::settlement_digest_u64(&digest, graph.instance_id());
    detail::settlement_digest_u64(&digest, graph.revision());
    detail::settlement_digest_u64(
        &digest, graph.history_deal_evidence_revision(deal_ticket));
    detail::settlement_digest_u64(&digest, deal->ticket);
    detail::settlement_digest_u64(&digest, deal->order_ticket);
    detail::settlement_digest_u64(&digest, deal->position_id);
    std::uint64_t volume_bits = 0;
    static_assert(sizeof(volume_bits) == sizeof(deal->volume), "unexpected double size");
    std::memcpy(&volume_bits, &deal->volume, sizeof(volume_bits));
    detail::settlement_digest_u64(&digest, volume_bits);
    if (digest == 0)
        return std::nullopt;

    ManagedDealSettlement result;
    result.key = {operation.key.account, operation.key, deal_ticket};
    result.source_operation_revision = operation.revision;
    result.managed_logical_units = managed_logical_units;
    result.provenance = {graph.instance_id(), graph.revision(),
                         graph.history_deal_evidence_revision(deal_ticket), digest};
    return result.valid() ? std::optional<ManagedDealSettlement>(std::move(result))
                          : std::nullopt;
}

/// \brief Builds a complete cumulative frontier from immutable per-deal facts.
/// \param operation Durable close operation at the frontier revision.
/// \param provenance Fresh proof that the entry set is complete.
/// \param facts Immutable facts contributing to this frontier.
/// \return A complete frontier, or empty when the sum/identity is ambiguous.
inline std::optional<ManagedSettlementFrontier> make_managed_settlement_frontier(
    const OperationRecord &operation, ManagedSettlementProvenance provenance,
    const std::vector<ManagedDealSettlement> &facts) {
    if (!operation.valid() || operation.operation_kind != OperationKind::close ||
        (operation.operation_state != OperationState::partially_filled &&
         operation.operation_state != OperationState::filled) ||
        operation.settled_volume == 0 || !provenance.valid() || facts.empty())
        return std::nullopt;

    ManagedSettlementFrontier frontier;
    frontier.account = operation.key.account;
    frontier.operation_key = operation.key;
    frontier.operation_revision = operation.revision;
    frontier.settled_volume = operation.settled_volume;
    frontier.provenance = provenance;
    for (const auto &fact : facts) {
        if (!fact.valid() || fact.key.operation_key != operation.key ||
            fact.source_operation_revision > operation.revision)
            return std::nullopt;
        frontier.entries.push_back(
            {fact.key.deal_ticket, fact.source_operation_revision,
             fact.managed_logical_units});
    }
    return frontier.valid() ? std::optional<ManagedSettlementFrontier>(std::move(frontier))
                            : std::nullopt;
}

/// \class ManagedDealSettlementProducer
/// \brief Creates sealed fact/frontier commits from authoritative reconciliation.
class ManagedDealSettlementProducer {
public:
    /// \brief Derives one new fact and the complete frontier that includes it.
    /// \param operation Durable close operation at the new frontier revision.
    /// \param baseline Durable reconciliation baseline bound to the operation.
    /// \param graph Fresh account-scoped observation graph.
    /// \param deal_ticket Newly attributed broker deal.
    /// \param managed_logical_units Owner-authoritative logical delta for the deal.
    /// \param prior_facts Earlier immutable facts referenced by the new frontier.
    /// \return Sealed store input, or empty when evidence is incomplete.
    static std::optional<ManagedDealSettlementCommit> create(
        const OperationRecord &operation, const ReconciliationBaseline &baseline,
        const ObservationGraph &graph, std::uint64_t deal_ticket,
        std::uint64_t managed_logical_units,
        const std::vector<ManagedDealSettlement> &prior_facts = {}) {
        const auto fact = derive_managed_deal_settlement(
            operation, baseline, graph, deal_ticket, managed_logical_units, prior_facts);
        if (!fact)
            return std::nullopt;
        auto facts = prior_facts;
        facts.push_back(*fact);
        const auto frontier = make_managed_settlement_frontier(
            operation, fact->provenance, facts);
        if (!frontier)
            return std::nullopt;
        return ManagedDealSettlementCommit(*fact, *frontier);
    }
};

} // namespace mt5bridge::dispatch

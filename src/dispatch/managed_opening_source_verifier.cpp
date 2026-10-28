/// \file managed_opening_source_verifier.cpp
/// \brief Implements fenced authoritative OPEN source verification.

#include "managed_opening_source_verifier.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace mt5bridge::dispatch {
namespace {

constexpr std::uint64_t kRequiredDealFields =
    MT5BRIDGE_DEAL_KNOWN_TICKET | MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
    MT5BRIDGE_DEAL_KNOWN_POSITION_ID | MT5BRIDGE_DEAL_KNOWN_TYPE |
    MT5BRIDGE_DEAL_KNOWN_ENTRY | MT5BRIDGE_DEAL_KNOWN_VOLUME |
    MT5BRIDGE_DEAL_KNOWN_TIME;

class SourceDigest {
public:
    void add_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            value_ ^= static_cast<std::uint8_t>(value >> shift);
            value_ *= 1099511628211ULL;
        }
    }

    void add_bytes(const std::vector<std::uint8_t> &bytes) {
        add_u64(static_cast<std::uint64_t>(bytes.size()));
        for (const auto byte : bytes) {
            value_ ^= byte;
            value_ *= 1099511628211ULL;
        }
    }

    void add_text(const std::string &value) {
        add_u64(static_cast<std::uint64_t>(value.size()));
        for (const auto byte : value) {
            value_ ^= static_cast<std::uint8_t>(byte);
            value_ *= 1099511628211ULL;
        }
    }

    std::uint64_t value() const { return value_ == 0 ? 1 : value_; }

private:
    std::uint64_t value_ = 1469598103934665603ULL;
};

std::optional<BrokerPositionDirection> direction_for_type(std::uint32_t type) {
    // MT5 DEAL_TYPE_BUY/POSITION_TYPE_BUY are 0; SELL values are 1.
    if (type == 0)
        return BrokerPositionDirection::buy;
    if (type == 1)
        return BrokerPositionDirection::sell;
    return std::nullopt;
}

const Mt5DealSnapshot *find_deal(const std::vector<Mt5DealSnapshot> &deals,
                                 std::uint64_t ticket) {
    for (const auto &deal : deals)
        if (deal.ticket == ticket)
            return &deal;
    return nullptr;
}

bool same_window(const ObservationWindow &left, const ObservationWindow &right) {
    return left.from_msc == right.from_msc && left.to_msc == right.to_msc;
}

ManagedOpeningSourceVerificationResult invalid_evidence() {
    return {ManagedOpeningSourceVerificationStatus::invalid_evidence, std::nullopt};
}

} // namespace

ManagedOpeningSourceVerificationResult ManagedOpeningSourceVerifier::verify_locked(
    const ManagedOpeningProvenanceRequest &request) const {
    const auto &journal_store = journal_store_;
    const auto &fact_store = fact_store_;
    const auto &graph = graph_;
    if (!quantity_domain_.valid() || !graph.bound() ||
        graph.account_key() != request.operation_key.account)
        return invalid_evidence();

    const auto journal = journal_store.load(request.operation_key);
    switch (journal.status) {
    case StoreLoadStatus::not_found:
        return {ManagedOpeningSourceVerificationStatus::unavailable, std::nullopt};
    case StoreLoadStatus::invalid_record:
        return invalid_evidence();
    case StoreLoadStatus::io_error:
        return {ManagedOpeningSourceVerificationStatus::io_error, std::nullopt};
    case StoreLoadStatus::found:
        break;
    }
    if (!journal.record || !journal.record->valid())
        return invalid_evidence();
    const auto &record = *journal.record;
    if (record.key != request.operation_key || record.operation_kind != OperationKind::open ||
        record.revision != request.operation_revision ||
        !journal_at_least_result_persisted(record.journal_state) ||
        record.result_payload.empty() ||
        (record.operation_state != OperationState::accepted &&
         record.operation_state != OperationState::reconciling &&
         record.operation_state != OperationState::partially_filled &&
         record.operation_state != OperationState::filled) ||
        !record.reconciliation_descriptor)
        return invalid_evidence();
    const auto &descriptor = *record.reconciliation_descriptor;
    if (!descriptor.valid() || descriptor.account != request.operation_key.account ||
        descriptor.trade_id != request.operation_key.trade_id ||
        descriptor.operation_id != request.operation_key.operation_id)
        return invalid_evidence();

    std::vector<std::uint64_t> bound_deal_tickets;
    std::optional<ObservationWindow> declared_window;
    std::set<std::uint64_t> distinct_bound_tickets;
    bool has_result_derived_binding = false;
    for (const auto &predicate : descriptor.predicates) {
        if (predicate.kind != ReconciliationPredicateKind::history_deal_present)
            continue;
        if (!predicate.history_window || !predicate.history_window->valid() ||
            !predicate.baseline_present || *predicate.baseline_present ||
            predicate.expected_transition != ReconciliationTransition::absent_to_present)
            return invalid_evidence();
        if (declared_window && !same_window(*declared_window, *predicate.history_window))
            return invalid_evidence();
        declared_window = predicate.history_window;

        std::uint64_t ticket = predicate.ticket;
        if (ticket == 0) {
            has_result_derived_binding = true;
            const auto binding = std::find_if(
                record.reconciliation_bindings.begin(),
                record.reconciliation_bindings.end(),
                [&predicate](const auto &candidate) {
                    return candidate.correlation_id == predicate.correlation_id;
                });
            if (binding == record.reconciliation_bindings.end() || !binding->valid())
                return invalid_evidence();
            ticket = binding->broker_ticket;
        }
        if (ticket == 0 || !distinct_bound_tickets.insert(ticket).second)
            return invalid_evidence();
        bound_deal_tickets.push_back(ticket);
    }
    // The opening source must be anchored by a result-derived history deal.
    if (!has_result_derived_binding || bound_deal_tickets.empty() || !declared_window)
        return invalid_evidence();

    const auto history_deals = graph.history_deals();
    const auto history_revision =
        graph.domain_revision(ObservationDomain::history_deals);
    if (history_revision == 0 ||
        !graph.history_deals_covered_at(*declared_window, history_revision))
        return invalid_evidence();

    std::uint64_t order_ticket = 0;
    std::optional<BrokerPositionDirection> direction;
    for (const auto ticket : bound_deal_tickets) {
        const auto *deal = find_deal(history_deals, ticket);
        if (!deal || (deal->known_fields & kRequiredDealFields) != kRequiredDealFields ||
            deal->entry != static_cast<std::uint32_t>(BrokerDealEntry::in) ||
            deal->position_id != request.position_identifier ||
            deal->time_msc < declared_window->from_msc ||
            deal->time_msc > declared_window->to_msc ||
            graph.history_deal_evidence_revision(ticket) == 0 ||
            !graph.history_deals_covered_at(
                *declared_window, graph.history_deal_evidence_revision(ticket)))
            return invalid_evidence();
        const auto candidate_direction = direction_for_type(deal->type);
        if (!candidate_direction)
            return invalid_evidence();
        if (order_ticket == 0)
            order_ticket = deal->order_ticket;
        if (deal->order_ticket != order_ticket)
            return invalid_evidence();
        if (direction && *direction != *candidate_direction)
            return invalid_evidence();
        direction = candidate_direction;
    }
    if (order_ticket == 0 || !direction)
        return invalid_evidence();

    const auto authoritative_deals = graph.deals_for_order(order_ticket);
    if (authoritative_deals.empty())
        return invalid_evidence();
    std::map<std::uint64_t, const Mt5DealSnapshot *> deals_by_ticket;
    for (const auto &deal : authoritative_deals) {
        if ((deal.known_fields & kRequiredDealFields) != kRequiredDealFields ||
            deal.entry != static_cast<std::uint32_t>(BrokerDealEntry::in) ||
            deal.position_id != request.position_identifier ||
            deal.time_msc < declared_window->from_msc ||
            deal.time_msc > declared_window->to_msc ||
            direction_for_type(deal.type) != direction ||
            graph.history_deal_evidence_revision(deal.ticket) == 0 ||
            !graph.history_deals_covered_at(
                *declared_window, graph.history_deal_evidence_revision(deal.ticket)) ||
            !deals_by_ticket.emplace(deal.ticket, &deal).second)
            return invalid_evidence();
    }
    const auto positions = graph.positions_for_identifier(request.position_identifier);
    if (positions.size() > 1)
        return invalid_evidence();
    if (positions.size() == 1 &&
        (positions.front().known_fields & MT5BRIDGE_POSITION_KNOWN_TYPE) != 0) {
        const auto position_direction = direction_for_type(positions.front().type);
        if (!position_direction || position_direction != direction)
            return invalid_evidence();
    }
    for (const auto &deal : graph.deals_for_position(request.position_identifier)) {
        if (deal.time_msc < declared_window->from_msc ||
            deal.time_msc > declared_window->to_msc)
            continue;
        if (deal.order_ticket != order_ticket)
            return invalid_evidence();
    }
    for (const auto ticket : bound_deal_tickets)
        if (deals_by_ticket.find(ticket) == deals_by_ticket.end())
            return invalid_evidence();

    const auto facts_scan = fact_store.scan(request.operation_key.account);
    if (facts_scan.status == ManagedOpeningFactScanStatus::io_error)
        return {ManagedOpeningSourceVerificationStatus::io_error, std::nullopt};
    if (facts_scan.status != ManagedOpeningFactScanStatus::complete)
        return invalid_evidence();
    std::vector<ManagedOpeningDealFact> facts;
    std::set<std::uint64_t> fact_tickets;
    for (const auto &fact : facts_scan.facts) {
        if (fact.operation_key() != request.operation_key)
            continue;
        if (!fact.valid() || fact.operation_revision() != request.operation_revision ||
            fact.position_identifier() != request.position_identifier ||
            fact.order_ticket() != order_ticket ||
            !same_window(
                {fact.admission_receipt().history_window_from_msc,
                 fact.admission_receipt().history_window_to_msc},
                *declared_window) ||
            !fact_tickets.insert(fact.deal_ticket()).second)
            return invalid_evidence();
        const auto deal_it = deals_by_ticket.find(fact.deal_ticket());
        if (deal_it == deals_by_ticket.end())
            return invalid_evidence();
        const auto &deal = *deal_it->second;
        const auto deal_direction = direction_for_type(deal.type);
        const auto &quantity = fact.quantity();
        const bool quantity_domain_matches =
            quantity.canonical_scale() == quantity_domain_.canonical_scale &&
            quantity.broker_step_units() == quantity_domain_.broker_step_units &&
            quantity.integer_step_count() != 0 &&
            quantity.managed_logical_units() / quantity.integer_step_count() ==
                quantity_domain_.managed_logical_units_per_step;
        if (!deal_direction || !quantity_domain_matches ||
            fact.direction() != *deal_direction ||
            fact.entry() != BrokerDealEntry::in || fact.deal_time_msc() != deal.time_msc ||
            !matches_managed_opening_quantity(deal.volume, quantity))
            return invalid_evidence();
        facts.push_back(fact);
    }
    if (facts.empty() || facts.size() != deals_by_ticket.size())
        return invalid_evidence();
    std::sort(facts.begin(), facts.end(), [](const auto &left, const auto &right) {
        return left.deal_ticket() < right.deal_ticket();
    });
    const auto frontier = ManagedOpeningExecutionFrontier::create(facts);
    if (!frontier)
        return invalid_evidence();

    SourceDigest digest;
    digest.add_text("mt5bridge-managed-opening-source-v1");
    digest.add_text(request.operation_key.account.server);
    digest.add_u64(request.operation_key.account.login);
    digest.add_u64(request.operation_key.trade_id);
    digest.add_u64(request.operation_key.operation_id);
    digest.add_u64(request.operation_revision);
    digest.add_u64(request.position_identifier);
    digest.add_u64(order_ticket);
    digest.add_u64(static_cast<std::uint64_t>(declared_window->from_msc));
    digest.add_u64(static_cast<std::uint64_t>(declared_window->to_msc));
    std::vector<std::uint64_t> deal_tickets;
    deal_tickets.reserve(facts.size());
    for (const auto &fact : facts) {
        const auto *deal = deals_by_ticket.at(fact.deal_ticket());
        digest.add_u64(deal->ticket);
        digest.add_u64(deal->order_ticket);
        digest.add_u64(deal->position_id);
        digest.add_u64(deal->type);
        digest.add_u64(deal->entry);
        std::uint64_t volume_bits = 0;
        std::memcpy(&volume_bits, &deal->volume, sizeof(volume_bits));
        digest.add_u64(volume_bits);
        digest.add_u64(static_cast<std::uint64_t>(deal->time_msc));
        digest.add_bytes(fact.canonical_bytes());
        deal_tickets.push_back(fact.deal_ticket());
    }
    VerifiedManagedOpeningSourceFact proof(
        request.operation_key, request.operation_revision,
        request.position_identifier, std::move(deal_tickets),
        frontier->cumulative_managed_logical_units(), digest.value());
    return proof.valid()
               ? ManagedOpeningSourceVerificationResult{
                     ManagedOpeningSourceVerificationStatus::found, std::move(proof)}
               : invalid_evidence();
}

ManagedOpeningSourceVerificationResult ManagedOpeningSourceVerifier::verify(
    const ManagedOpeningProvenanceRequest &request) const {
    if (!request.valid())
        return {ManagedOpeningSourceVerificationStatus::invalid_request, std::nullopt};
    const auto token = lease_.held_fencing_token(request.operation_key.account);
    if (!token)
        return {ManagedOpeningSourceVerificationStatus::lease_not_held, std::nullopt};

    ManagedOpeningSourceVerificationResult result{
        ManagedOpeningSourceVerificationStatus::lease_not_held, std::nullopt};
    const bool fenced = lease_.with_fencing_capability(
        request.operation_key.account, *token, [&] {
            result = verify_locked(request);
        });
    return fenced ? result
                  : ManagedOpeningSourceVerificationResult{
                        ManagedOpeningSourceVerificationStatus::lease_not_held,
                        std::nullopt};
}

} // namespace mt5bridge::dispatch

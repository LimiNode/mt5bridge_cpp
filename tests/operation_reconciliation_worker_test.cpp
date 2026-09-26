/// \file operation_reconciliation_worker_test.cpp
/// \brief Exercises startup classification and journal-aware reconciliation.

#include <mt5bridge.hpp>

#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

class MemoryStore final : public mt5bridge::DurableJournalStore {
public:
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        const auto it = records.find(record.key);
        if (expected_revision) {
            if (it == records.end() || it->second.revision != *expected_revision)
                return mt5bridge::StoreCommitStatus::conflict;
        } else if (it != records.end()) {
            return mt5bridge::StoreCommitStatus::conflict;
        }
        records[record.key] = record;
        return mt5bridge::StoreCommitStatus::committed;
    }

    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &key) const override {
        const auto it = records.find(key);
        return it == records.end()
                   ? mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::not_found,
                                                std::nullopt}
                   : mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::found,
                                                it->second};
    }

    mt5bridge::StoreScanResult scan() const override {
        mt5bridge::StoreScanResult result;
        result.status = mt5bridge::StoreScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> records;
};

class FakeProvider final : public mt5bridge::ObservationProvider {
public:
    explicit FakeProvider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &) override {
        if (next_ == batches_.size())
            throw std::runtime_error("provider exhausted");
        ++calls;
        return std::move(batches_[next_++]);
    }

    std::size_t calls = 0;

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

mt5bridge::ObservationBatch batch(const mt5bridge::AccountKey &key,
                                   bool include_order) {
    mt5bridge::ObservationBatch result;
    result.account = key;
    result.observed_domains = mt5bridge::ObservationDomain::active_orders;
    if (include_order) {
        Mt5OrderSnapshot order{};
        order.ticket = 100;
        order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                             MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
        result.active_orders.push_back(order);
    }
    return result;
}

mt5bridge::ObservationBatch history_batch(const mt5bridge::AccountKey &key,
                                          mt5bridge::ObservationWindow window,
                                          bool include_order) {
    mt5bridge::ObservationBatch result;
    result.account = key;
    result.observed_domains = mt5bridge::ObservationDomain::history_orders;
    result.history_orders_window = window;
    if (include_order) {
        Mt5HistoryOrderSnapshot order{};
        order.ticket = 100;
        order.position_id = 700;
        order.time_done_msc = window.from_msc;
        order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                             MT5BRIDGE_ORDER_KNOWN_POSITION_ID |
                             MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
        result.history_orders.push_back(order);
    }
    return result;
}

void enter_dispatching(mt5bridge::OperationJournal &journal,
                       const mt5bridge::OperationKey &key,
                       mt5bridge::ReconciliationDescriptor descriptor) {
    require(journal.transition_operation(key, mt5bridge::OperationState::prechecking)
                .accepted(),
            "operation did not enter prechecking");
    require(journal.transition_journal(key, mt5bridge::JournalState::prechecked)
                .accepted(),
            "journal did not enter prechecked");
    require(journal.transition_journal(
                key, mt5bridge::JournalState::dispatch_intent_persisted)
                .accepted(),
            "dispatch intent was not persisted");
    require(journal.persist_reconciliation_descriptor(key, std::move(descriptor))
                .accepted(),
            "reconciliation descriptor was not persisted");
    require(journal.transition_journal(key, mt5bridge::JournalState::dispatching, 77)
                .accepted(),
            "dispatch barrier was not persisted");
}

} // namespace

/// \brief Runs startup and operation reconciliation checks.
/// \return Zero on success; non-zero on invariant failure.
int main() {
    try {
        const auto key = mt5bridge::OperationKey{account(), 7, 11};
        MemoryStore store;
        mt5bridge::OperationJournal journal(store);
        require(journal.create(key, {0x01}).accepted(), "operation create failed");

        FakeProvider provider({batch(key.account, false), batch(key.account, false),
                               batch(key.account, true)});
        mt5bridge::ObservationCoordinator coordinator(provider, key.account);
        mt5bridge::ObservationCollectionRequest collection;
        collection.observe_positions = false;
        mt5bridge::ReconciliationRequest request;
        const auto baseline_refresh = coordinator.refresh(collection);
        require(baseline_refresh.apply.accepted(), "baseline refresh failed");
        request.baseline = coordinator.capture_baseline();
        request.predicates = {mt5bridge::require_active_order(100)};
        enter_dispatching(
            journal, key,
            mt5bridge::ReconciliationDescriptor{key.account, *request.baseline,
                                                 request.predicates,
                                                 mt5bridge::OperationState::filled});

        const auto recovered = mt5bridge::OperationRecoveryCoordinator::recover(journal);
        require(recovered.accepted() && recovered.operations.size() == 1 &&
                    recovered.operations.front().non_resendable() &&
                    recovered.operations.front().action ==
                        mt5bridge::OperationRecoveryAction::reconcile_only,
                "dispatching recovery became resendable");

        mt5bridge::OperationReconciliationWorker worker(
            journal, key, coordinator, collection, request);
        const auto first = worker.step();
        require(first.status == mt5bridge::OperationReconciliationStatus::pending &&
                    first.record && first.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    first.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "pending reconciliation was not kept unresolved");
        const auto second = worker.step();
        require(second.status == mt5bridge::OperationReconciliationStatus::progressed &&
                    second.record && second.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    second.record->journal_state == mt5bridge::JournalState::reconciling,
                "confirmed reconciliation was not durably settled");
        require(provider.calls == 3, "worker did not perform baseline and two cycles");

        const auto enrichment_key = mt5bridge::OperationKey{account(), 70, 71};
        require(journal.create(enrichment_key, {0x08}).accepted(),
                "ticket-enrichment operation create failed");
        FakeProvider enrichment_provider({batch(enrichment_key.account, false),
                                          batch(enrichment_key.account, true)});
        mt5bridge::ObservationCoordinator enrichment_coordinator(
            enrichment_provider, enrichment_key.account);
        mt5bridge::ObservationCollectionRequest enrichment_collection;
        enrichment_collection.observe_positions = false;
        require(enrichment_coordinator.refresh(enrichment_collection).apply.accepted(),
                "ticket-enrichment baseline refresh failed");
        const auto enrichment_baseline = enrichment_coordinator.capture_baseline();
        const auto unknown_predicate = mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::active_order_present,
            std::nullopt, false, mt5bridge::ReconciliationTransition::absent_to_present,
            7001);
        enter_dispatching(
            journal, enrichment_key,
            mt5bridge::ReconciliationDescriptor{enrichment_key.account,
                                                 enrichment_baseline,
                                                 {unknown_predicate},
                                                 mt5bridge::OperationState::filled});
        mt5bridge::ReconciliationRequest enriched_request;
        enriched_request.baseline = enrichment_baseline;
        enriched_request.predicates = {mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::active_order_present, 100, false,
            mt5bridge::ReconciliationTransition::absent_to_present, 7001)};
        mt5bridge::OperationReconciliationWorker enrichment_worker(
            journal, enrichment_key, enrichment_coordinator, enrichment_collection,
            enriched_request);
        const auto enriched_cycle = enrichment_worker.step();
        require(enriched_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::invalid_request,
                "caller-supplied ticket enriched an unbound identity");

        const auto restart_key = mt5bridge::OperationKey{account(), 13, 17};
        require(journal.create(restart_key, {0x07}).accepted(),
                "restart reconciliation operation create failed");
        enter_dispatching(
            journal, restart_key,
            mt5bridge::ReconciliationDescriptor{restart_key.account, *request.baseline,
                                                 request.predicates,
                                                 mt5bridge::OperationState::filled});
        mt5bridge::OperationJournal recovered_journal(store);
        require(recovered_journal.recover(restart_key).accepted(),
                "restart reconciliation operation was not recovered");
        FakeProvider restart_provider({batch(restart_key.account, false),
                                       batch(restart_key.account, true)});
        mt5bridge::ObservationCoordinator restart_coordinator(restart_provider,
                                                               restart_key.account);
        mt5bridge::ObservationCollectionRequest restart_collection;
        restart_collection.observe_positions = false;
        mt5bridge::OperationReconciliationWorker restart_worker(
            recovered_journal, restart_key, restart_coordinator, restart_collection);
        const auto restart_first = restart_worker.step();
        const auto restart_second = restart_worker.step();
        require(restart_first.status ==
                    mt5bridge::OperationReconciliationStatus::pending &&
                    restart_second.status ==
                        mt5bridge::OperationReconciliationStatus::progressed &&
                    restart_second.record &&
                    restart_second.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    restart_provider.calls == 2,
                "recovered descriptor did not re-anchor to the new graph instance");

        const auto unresolved_key = mt5bridge::OperationKey{account(), 8, 12};
        require(journal.create(unresolved_key, {0x02}).accepted(),
                "unresolved operation create failed");
        FakeProvider unresolved_provider(
            {batch(unresolved_key.account, false), batch(unresolved_key.account, false)});
        mt5bridge::ObservationCoordinator unresolved_coordinator(
            unresolved_provider, unresolved_key.account);
        mt5bridge::ObservationCollectionRequest unresolved_collection;
        unresolved_collection.observe_positions = false;
        require(unresolved_coordinator.refresh(unresolved_collection).apply.accepted(),
                "unresolved baseline refresh failed");
        mt5bridge::ReconciliationRequest unresolved_request;
        unresolved_request.baseline = unresolved_coordinator.capture_baseline();
        unresolved_request.predicates = {mt5bridge::require_active_order(200)};
        enter_dispatching(
            journal, unresolved_key,
            mt5bridge::ReconciliationDescriptor{unresolved_key.account,
                                                 *unresolved_request.baseline,
                                                 unresolved_request.predicates,
                                                 mt5bridge::OperationState::filled});
        mt5bridge::ReconciliationRequest mismatched_request = unresolved_request;
        mismatched_request.predicates = {mt5bridge::require_active_order(201)};
        mt5bridge::OperationReconciliationWorker mismatched_worker(
            journal, unresolved_key, unresolved_coordinator, unresolved_collection,
            mismatched_request);
        require(mismatched_worker.step().status ==
                    mt5bridge::OperationReconciliationStatus::invalid_request,
                "caller-supplied reconciliation contract replaced the durable descriptor");
        mt5bridge::OperationReconciliationWorker unresolved_worker(
            journal, unresolved_key, unresolved_coordinator, unresolved_collection);
        const auto not_observed = unresolved_worker.step(false, true);
        require(not_observed.status ==
                    mt5bridge::OperationReconciliationStatus::not_observed &&
                    not_observed.record &&
                    not_observed.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "not-observed evidence became a terminal lifecycle state");

        const auto gap_key = mt5bridge::OperationKey{account(), 9, 13};
        require(journal.create(gap_key, {0x03}).accepted(),
                "event-gap operation create failed");
        const mt5bridge::ObservationWindow gap_window{1000, 2000};
        FakeProvider gap_provider({history_batch(gap_key.account, gap_window, false),
                                   history_batch(gap_key.account, gap_window, false),
                                   history_batch(gap_key.account, gap_window, true)});
        mt5bridge::ObservationCoordinator gap_coordinator(gap_provider, gap_key.account);
        mt5bridge::ObservationCollectionRequest gap_collection;
        gap_collection.observe_active_orders = false;
        gap_collection.observe_positions = false;
        gap_collection.history_orders_window = gap_window;
        require(gap_coordinator.refresh(gap_collection).apply.accepted(),
                "event-gap baseline refresh failed");
        mt5bridge::ReconciliationRequest gap_request;
        gap_request.baseline = gap_coordinator.capture_baseline();
        gap_request.predicates = {mt5bridge::require_history_order(100, gap_window)};
        enter_dispatching(
            journal, gap_key,
            mt5bridge::ReconciliationDescriptor{gap_key.account, *gap_request.baseline,
                                                 gap_request.predicates,
                                                 mt5bridge::OperationState::filled});
        mt5bridge::OperationReconciliationWorker gap_worker(
            journal, gap_key, gap_coordinator, gap_collection, gap_request);
        const auto gap_cycle = gap_worker.step(true, false);
        require(gap_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::trade_event_gap &&
                    gap_cycle.record &&
                    gap_cycle.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "event gap settled or changed the operation lifecycle");
        const auto gap_confirmation = gap_worker.step();
        require(gap_confirmation.status ==
                    mt5bridge::OperationReconciliationStatus::progressed,
                "authoritative refresh did not recover from an event gap");

        const auto account_key = mt5bridge::OperationKey{account(), 10, 14};
        require(journal.create(account_key, {0x04}).accepted(),
                "account-mismatch operation create failed");
        FakeProvider account_provider({batch(account_key.account, false),
                                       batch({"Other-Server", 43}, false)});
        mt5bridge::ObservationCoordinator account_coordinator(account_provider,
                                                               account_key.account);
        mt5bridge::ObservationCollectionRequest account_collection;
        account_collection.observe_positions = false;
        require(account_coordinator.refresh(account_collection).apply.accepted(),
                "account-mismatch baseline refresh failed");
        mt5bridge::ReconciliationRequest account_request;
        account_request.baseline = account_coordinator.capture_baseline();
        account_request.predicates = {mt5bridge::require_active_order(100)};
        enter_dispatching(
            journal, account_key,
            mt5bridge::ReconciliationDescriptor{account_key.account,
                                                 *account_request.baseline,
                                                 account_request.predicates,
                                                 mt5bridge::OperationState::filled});
        mt5bridge::OperationReconciliationWorker account_worker(
            journal, account_key, account_coordinator, account_collection, account_request);
        const auto account_cycle = account_worker.step();
        require(account_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::account_mismatch &&
                    account_cycle.record &&
                    account_cycle.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    account_cycle.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "account mismatch was not suspended in reconciliation");
        const auto suspended_cycle = account_worker.step();
        require(suspended_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::account_mismatch &&
                    account_provider.calls == 2,
                "account-mismatched worker resumed observation instead of suspending");

        const auto ambiguous_key = mt5bridge::OperationKey{account(), 11, 15};
        require(journal.create(ambiguous_key, {0x05}).accepted(),
                "ambiguous operation create failed");
        mt5bridge::ObservationBatch malformed = batch(ambiguous_key.account, false);
        Mt5OrderSnapshot malformed_order{};
        malformed_order.ticket = 100;
        malformed_order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET;
        malformed.active_orders.push_back(malformed_order);
        FakeProvider ambiguous_provider({batch(ambiguous_key.account, false),
                                         std::move(malformed)});
        mt5bridge::ObservationCoordinator ambiguous_coordinator(ambiguous_provider,
                                                                  ambiguous_key.account);
        mt5bridge::ObservationCollectionRequest ambiguous_collection;
        ambiguous_collection.observe_positions = false;
        require(ambiguous_coordinator.refresh(ambiguous_collection).apply.accepted(),
                "ambiguous baseline refresh failed");
        mt5bridge::ReconciliationRequest ambiguous_request;
        ambiguous_request.baseline = ambiguous_coordinator.capture_baseline();
        ambiguous_request.predicates = {mt5bridge::require_active_order(100)};
        enter_dispatching(
            journal, ambiguous_key,
            mt5bridge::ReconciliationDescriptor{ambiguous_key.account,
                                                 *ambiguous_request.baseline,
                                                 ambiguous_request.predicates,
                                                 mt5bridge::OperationState::filled});
        mt5bridge::OperationReconciliationWorker ambiguous_worker(
            journal, ambiguous_key, ambiguous_coordinator, ambiguous_collection,
            ambiguous_request);
        const auto ambiguous_cycle = ambiguous_worker.step();
        require(ambiguous_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::ambiguous &&
                    ambiguous_cycle.record &&
                    ambiguous_cycle.record->operation_state ==
                        mt5bridge::OperationState::ambiguous &&
                    ambiguous_cycle.record->journal_state ==
                        mt5bridge::JournalState::reconciling,
                "ambiguous evidence was not durably settled as ambiguous");

        const auto partial_key = mt5bridge::OperationKey{account(), 12, 16};
        require(journal.create(partial_key, {0x06}).accepted(),
                "partial operation create failed");
        FakeProvider partial_provider({batch(partial_key.account, false),
                                       batch(partial_key.account, true)});
        mt5bridge::ObservationCoordinator partial_coordinator(partial_provider,
                                                                partial_key.account);
        mt5bridge::ObservationCollectionRequest partial_collection;
        partial_collection.observe_positions = false;
        require(partial_coordinator.refresh(partial_collection).apply.accepted(),
                "partial baseline refresh failed");
        mt5bridge::ReconciliationRequest partial_request;
        partial_request.baseline = partial_coordinator.capture_baseline();
        partial_request.predicates = {mt5bridge::require_active_order(100)};
        enter_dispatching(
            journal, partial_key,
            mt5bridge::ReconciliationDescriptor{partial_key.account,
                                                 *partial_request.baseline,
                                                 partial_request.predicates,
                                                 mt5bridge::OperationState::partially_filled});
        mt5bridge::OperationReconciliationWorker partial_worker(
            journal, partial_key, partial_coordinator, partial_collection,
            partial_request, mt5bridge::OperationState::partially_filled);
        const auto partial_cycle = partial_worker.step();
        require(partial_cycle.status ==
                    mt5bridge::OperationReconciliationStatus::progressed &&
                    partial_cycle.record &&
                    partial_cycle.record->operation_state ==
                        mt5bridge::OperationState::partially_filled,
                "partial settlement state was not preserved");

        MemoryStore restart_store = store;
        mt5bridge::OperationJournal restarted(restart_store);
        const auto restarted_recovery =
            mt5bridge::OperationRecoveryCoordinator::recover(restarted);
        require(restarted_recovery.accepted() &&
                    restarted_recovery.operations.front().action ==
                        mt5bridge::OperationRecoveryAction::terminal,
                "filled operation was not classified terminal after restart");

        std::cout << "operation reconciliation worker checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "operation reconciliation worker checks failed: " << error.what()
                  << '\n';
        return 1;
    }
}

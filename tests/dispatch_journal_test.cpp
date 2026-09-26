/// \file dispatch_journal_test.cpp
/// \brief Exercises durable operation journal and pre-side-effect admission.

#include <mt5bridge.hpp>

#include <cstdlib>
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

mt5bridge::AccountKey account(std::uint64_t login = 42) {
    return {"Demo-Trade", login};
}

class MemoryStore final : public mt5bridge::DurableJournalStore {
public:
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        const auto it = durable.find(record.key);
        if (expected_revision.has_value()) {
            if (it == durable.end() || it->second.revision != *expected_revision)
                return mt5bridge::StoreCommitStatus::conflict;
        } else if (it != durable.end()) {
            return mt5bridge::StoreCommitStatus::conflict;
        }
        if (fail_next) {
            fail_next = false;
            return mt5bridge::StoreCommitStatus::io_error;
        }
        durable[record.key] = record;
        commits.push_back(record);
        return mt5bridge::StoreCommitStatus::committed;
    }

    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &key) const override {
        if (load_status != mt5bridge::StoreLoadStatus::found)
            return {load_status, std::nullopt};
        const auto it = durable.find(key);
        return it == durable.end()
                   ? mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::not_found,
                                                std::nullopt}
                   : mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::found, it->second};
    }

    mt5bridge::StoreScanResult scan() const override {
        mt5bridge::StoreScanResult result;
        result.status = scan_status;
        if (scan_status != mt5bridge::StoreScanStatus::complete)
            return result;
        for (const auto &entry : durable)
            result.records.push_back(entry.second);
        return result;
    }

    bool fail_next = false;
    mt5bridge::StoreLoadStatus load_status = mt5bridge::StoreLoadStatus::found;
    mt5bridge::StoreScanStatus scan_status = mt5bridge::StoreScanStatus::complete;
    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> durable;
    std::vector<mt5bridge::OperationRecord> commits;
};

class FakeLease final : public mt5bridge::SingleWriterLease {
public:
    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &account) const override {
        if (!held || account != owned_account)
            return std::nullopt;
        return token;
    }

    mt5bridge::AccountKey owned_account;
    std::uint64_t token = 0;
    bool held = false;
};

class FakeObservationProvider final : public mt5bridge::ObservationProvider {
public:
    explicit FakeObservationProvider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &) override {
        if (next_ == batches_.size())
            throw std::runtime_error("observation provider exhausted");
        return std::move(batches_[next_++]);
    }

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

Mt5OrderSnapshot active_order() {
    Mt5OrderSnapshot value{};
    value.ticket = 20;
    value.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    return value;
}

mt5bridge::ObservationBatch active_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    batch.active_orders.push_back(active_order());
    return batch;
}

mt5bridge::ObservationBatch empty_active_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    return batch;
}

mt5bridge::ObservationBatch positions_only_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::positions;
    return batch;
}

mt5bridge::ObservationBatch history_orders_batch(
    const mt5bridge::AccountKey &key, mt5bridge::ObservationWindow window,
    std::vector<Mt5HistoryOrderSnapshot> orders = {}) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::history_orders;
    batch.history_orders_window = window;
    batch.history_orders = std::move(orders);
    return batch;
}

Mt5HistoryOrderSnapshot history_order(std::uint64_t ticket,
                                      std::int64_t time_done_msc) {
    Mt5HistoryOrderSnapshot value{};
    value.ticket = ticket;
    value.time_done_msc = time_done_msc;
    value.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID |
                         MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
    return value;
}

mt5bridge::DispatchAdmissionRequest ready_request(
    const mt5bridge::AccountKey &key,
    const mt5bridge::EnvironmentConsistencyProof &proof) {
    mt5bridge::DispatchAdmissionRequest request;
    request.current_account = key;
    request.environment_proof = proof;
    return request;
}

void prepare_dispatch_intent(mt5bridge::OperationJournal &journal,
                             const mt5bridge::OperationKey &key) {
    require(journal.transition_operation(key, mt5bridge::OperationState::prechecking)
                .accepted(),
            "prechecking operation state was rejected");
    require(journal.transition_journal(key, mt5bridge::JournalState::prechecked)
                .accepted(),
            "prechecked journal state was rejected");
    require(journal.transition_journal(
                key, mt5bridge::JournalState::dispatch_intent_persisted)
                .accepted(),
            "dispatch intent was not durably persisted");
}

void prepare_for_admission(
    mt5bridge::OperationJournal &journal, const mt5bridge::OperationKey &key,
    mt5bridge::ReconciliationDescriptor descriptor) {
    prepare_dispatch_intent(journal, key);
    require(journal.persist_reconciliation_descriptor(key, std::move(descriptor))
                .accepted(),
            "reconciliation descriptor was not durably persisted");
}

mt5bridge::ReconciliationDescriptor descriptor_for(
    const mt5bridge::OperationKey &key, const mt5bridge::ObservationGraph &graph,
    mt5bridge::ReconciliationPredicate predicate = mt5bridge::require_active_order(999)) {
    return {key.account, mt5bridge::capture_reconciliation_baseline(graph),
            {std::move(predicate)}, mt5bridge::OperationState::filled};
}

} // namespace

/// \brief Runs durable journal and dispatch-admission contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = mt5bridge::OperationKey{account(), 7, 11};

        FakeObservationProvider provider(
            {active_batch(key.account), active_batch(key.account), active_batch(key.account)});
        mt5bridge::ObservationCoordinator coordinator(provider, key.account);
        mt5bridge::ObservationCollectionRequest collection;
        const auto first_refresh = coordinator.refresh(collection);
        const auto second_refresh = coordinator.refresh(collection);
        const auto third_refresh = coordinator.refresh(collection);
        require(first_refresh.sample && second_refresh.sample && third_refresh.sample,
                "observation proof setup did not produce samples");
        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        const auto initial_consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first_refresh.sample, *second_refresh.sample}, consistency_request);
        require(initial_consistency.consistent() && initial_consistency.proof.has_value(),
                "consistent policy result did not carry an opaque proof");
        const auto fresh_consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*second_refresh.sample, *third_refresh.sample}, consistency_request);
        require(fresh_consistency.consistent() && fresh_consistency.proof.has_value(),
                "fresh consistent policy result did not carry a proof");

        mt5bridge::EnvironmentConsistencyRequest positions_scope;
        positions_scope.require_active_orders = false;
        FakeObservationProvider positions_provider(
            {positions_only_batch(key.account), positions_only_batch(key.account)});
        mt5bridge::ObservationCoordinator positions_coordinator(positions_provider,
                                                                 key.account);
        mt5bridge::ObservationCollectionRequest positions_collection;
        positions_collection.observe_active_orders = false;
        const auto positions_first = positions_coordinator.refresh(positions_collection);
        const auto positions_second = positions_coordinator.refresh(positions_collection);
        require(positions_first.sample && positions_second.sample,
                "positions-only proof setup did not produce samples");
        const auto positions_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*positions_first.sample, *positions_second.sample}, positions_scope);
        require(positions_consistency.consistent() && positions_consistency.proof.has_value(),
                "positions-only policy result did not carry a proof");

        FakeObservationProvider stale_active_provider(
            {empty_active_batch(key.account), positions_only_batch(key.account),
             positions_only_batch(key.account), positions_only_batch(key.account),
             positions_only_batch(key.account)});
        mt5bridge::ObservationCoordinator stale_active_coordinator(stale_active_provider,
                                                                    key.account);
        require(stale_active_coordinator.refresh(collection).sample.has_value(),
                "stale active-order setup did not accept its initial sample");
        const auto stale_active_second =
            stale_active_coordinator.refresh(positions_collection);
        const auto stale_active_third =
            stale_active_coordinator.refresh(positions_collection);
        const auto stale_active_fourth =
            stale_active_coordinator.refresh(positions_collection);
        const auto stale_active_fifth =
            stale_active_coordinator.refresh(positions_collection);
        require(stale_active_second.sample && stale_active_third.sample &&
                    stale_active_fourth.sample && stale_active_fifth.sample,
                "stale active-order setup did not advance positions");
        const auto stale_active_baseline = stale_active_coordinator.capture_baseline();
        require(stale_active_baseline.active_orders_revision() != 0 &&
                    stale_active_baseline.active_orders_revision() <
                        stale_active_coordinator.graph().revision(),
                "stale active-order setup did not preserve an old domain revision");
        const auto stale_active_descriptor = mt5bridge::ReconciliationDescriptor{
            key.account, stale_active_baseline,
            {mt5bridge::require_active_order(100)}, mt5bridge::OperationState::filled};
        require(mt5bridge::reconciliation_descriptor_matches_graph(
                    stale_active_descriptor, stale_active_coordinator.graph()),
                "stale active-order descriptor setup unexpectedly failed");
        const auto stale_active_proof =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*stale_active_fourth.sample, *stale_active_fifth.sample},
                positions_scope);
        require(stale_active_proof.consistent(),
                "stale active-order positions proof setup failed");
        const auto stale_active_key = mt5bridge::OperationKey{account(), 34, 35};
        MemoryStore stale_active_store;
        mt5bridge::OperationJournal stale_active_journal(stale_active_store);
        require(stale_active_journal.create(stale_active_key, {0x0F}).accepted(),
                "stale active-order operation setup failed");
        prepare_for_admission(stale_active_journal, stale_active_key,
                              stale_active_descriptor);
        FakeLease stale_active_lease;
        stale_active_lease.owned_account = stale_active_key.account;
        stale_active_lease.token = 84;
        stale_active_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier stale_active_barrier(
            stale_active_journal, stale_active_coordinator.graph(), positions_scope);
        require(stale_active_barrier
                    .admit(stale_active_key,
                          ready_request(stale_active_key.account,
                                        *stale_active_proof.proof),
                          stale_active_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "stale active-order domain was admitted from a positions-only proof");

        const mt5bridge::ObservationWindow history_window{1000, 2000};
        FakeObservationProvider history_provider(
            {history_orders_batch(key.account, history_window),
             history_orders_batch(key.account, history_window),
             history_orders_batch(key.account, {0, 3000},
                                  {history_order(500, 1500)})});
        mt5bridge::ObservationCoordinator history_coordinator(history_provider,
                                                               key.account);
        mt5bridge::ObservationCollectionRequest history_collection;
        history_collection.observe_active_orders = false;
        history_collection.observe_positions = false;
        history_collection.history_orders_window = history_window;
        const auto history_first = history_coordinator.refresh(history_collection);
        const auto history_second = history_coordinator.refresh(history_collection);
        require(history_first.sample && history_second.sample,
                "history coverage setup did not produce samples");
        mt5bridge::EnvironmentConsistencyRequest history_scope;
        history_scope.require_active_orders = false;
        history_scope.require_positions = false;
        history_scope.history_orders_window = history_window;
        const auto history_consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*history_first.sample, *history_second.sample}, history_scope);
        require(history_consistency.consistent() && history_consistency.proof.has_value(),
                "history consistency proof setup failed");

        FakeObservationProvider stale_history_provider(
            {history_orders_batch(key.account, history_window),
             positions_only_batch(key.account), positions_only_batch(key.account),
             positions_only_batch(key.account), positions_only_batch(key.account)});
        mt5bridge::ObservationCoordinator stale_history_coordinator(stale_history_provider,
                                                                     key.account);
        mt5bridge::ObservationCollectionRequest stale_history_collection;
        stale_history_collection.observe_active_orders = false;
        stale_history_collection.observe_positions = false;
        stale_history_collection.history_orders_window = history_window;
        require(stale_history_coordinator.refresh(stale_history_collection).sample.has_value(),
                "stale history setup did not accept history sample");
        const auto stale_history_second =
            stale_history_coordinator.refresh(positions_collection);
        const auto stale_history_third =
            stale_history_coordinator.refresh(positions_collection);
        const auto stale_history_fourth =
            stale_history_coordinator.refresh(positions_collection);
        const auto stale_history_fifth =
            stale_history_coordinator.refresh(positions_collection);
        require(stale_history_second.sample && stale_history_third.sample &&
                    stale_history_fourth.sample && stale_history_fifth.sample,
                "stale history setup did not advance unrelated positions");
        const auto stale_history_baseline = stale_history_coordinator.capture_baseline();
        const auto stale_history_descriptor = mt5bridge::ReconciliationDescriptor{
            key.account, stale_history_baseline,
            {mt5bridge::require_history_order(500, history_window)},
            mt5bridge::OperationState::filled};
        require(mt5bridge::reconciliation_descriptor_matches_graph(
                    stale_history_descriptor, stale_history_coordinator.graph()),
                "stale history descriptor setup unexpectedly failed");
        const auto stale_history_proof =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*stale_history_fourth.sample, *stale_history_fifth.sample},
                positions_scope);
        require(stale_history_proof.consistent(),
                "stale history positions proof setup failed");
        const auto stale_history_key = mt5bridge::OperationKey{account(), 36, 37};
        MemoryStore stale_history_store;
        mt5bridge::OperationJournal stale_history_journal(stale_history_store);
        require(stale_history_journal.create(stale_history_key, {0x10}).accepted(),
                "stale history operation setup failed");
        prepare_for_admission(stale_history_journal, stale_history_key,
                              stale_history_descriptor);
        FakeLease stale_history_lease;
        stale_history_lease.owned_account = stale_history_key.account;
        stale_history_lease.token = 85;
        stale_history_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier stale_history_barrier(
            stale_history_journal, stale_history_coordinator.graph(), positions_scope);
        require(stale_history_barrier
                    .admit(stale_history_key,
                          ready_request(stale_history_key.account,
                                        *stale_history_proof.proof),
                          stale_history_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "stale history domain was admitted from a positions-only proof");

        const auto unobserved_history_descriptor = mt5bridge::ReconciliationDescriptor{
            key.account, mt5bridge::capture_reconciliation_baseline(coordinator.graph()),
            {mt5bridge::require_history_order(500, history_window)},
            mt5bridge::OperationState::filled};
        require(unobserved_history_descriptor.valid(),
                "history baseline-absence descriptor was rejected before coverage check");
        require(!mt5bridge::reconciliation_descriptor_matches_graph(
                    unobserved_history_descriptor, coordinator.graph()),
                "unobserved history absence was treated as proven baseline evidence");
        const auto unobserved_active_descriptor = mt5bridge::ReconciliationDescriptor{
            key.account, mt5bridge::capture_reconciliation_baseline(
                             positions_coordinator.graph()),
            {mt5bridge::require_active_order(999)}, mt5bridge::OperationState::filled};
        require(!mt5bridge::reconciliation_descriptor_matches_graph(
                    unobserved_active_descriptor, positions_coordinator.graph()),
                "unobserved active-order absence was treated as proven baseline evidence");
        const auto unobserved_active_key = mt5bridge::OperationKey{account(), 32, 33};
        MemoryStore unobserved_active_store;
        mt5bridge::OperationJournal unobserved_active_journal(unobserved_active_store);
        require(unobserved_active_journal.create(unobserved_active_key, {0x0E}).accepted(),
                "unobserved active-order operation setup failed");
        prepare_for_admission(unobserved_active_journal, unobserved_active_key,
                              unobserved_active_descriptor);
        FakeLease unobserved_active_lease;
        unobserved_active_lease.owned_account = unobserved_active_key.account;
        unobserved_active_lease.token = 83;
        unobserved_active_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier unobserved_active_barrier(
            unobserved_active_journal, positions_coordinator.graph(), positions_scope);
        require(unobserved_active_barrier
                    .admit(unobserved_active_key,
                          ready_request(unobserved_active_key.account,
                                        *positions_consistency.proof),
                          unobserved_active_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "unobserved active-order predicate opened the dispatch barrier");
        const auto unobserved_history_key = mt5bridge::OperationKey{account(), 30, 31};
        MemoryStore unobserved_history_store;
        mt5bridge::OperationJournal unobserved_history_journal(unobserved_history_store);
        require(unobserved_history_journal.create(unobserved_history_key, {0x0D}).accepted(),
                "unobserved history operation setup failed");
        prepare_for_admission(unobserved_history_journal, unobserved_history_key,
                              unobserved_history_descriptor);
        FakeLease unobserved_history_lease;
        unobserved_history_lease.owned_account = unobserved_history_key.account;
        unobserved_history_lease.token = 82;
        unobserved_history_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier unobserved_history_barrier(
            unobserved_history_journal, coordinator.graph(), consistency_request);
        require(unobserved_history_barrier
                    .admit(unobserved_history_key,
                          ready_request(unobserved_history_key.account,
                                        *fresh_consistency.proof),
                          unobserved_history_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "unobserved history predicate opened the dispatch barrier");

        const auto covered_history_descriptor = mt5bridge::ReconciliationDescriptor{
            key.account, mt5bridge::capture_reconciliation_baseline(history_coordinator.graph()),
            {mt5bridge::require_history_order(500, history_window)},
            mt5bridge::OperationState::filled};
        require(mt5bridge::reconciliation_descriptor_matches_graph(
                    covered_history_descriptor, history_coordinator.graph()),
                "covered history baseline absence was rejected unexpectedly");
        auto wider_history_collection = history_collection;
        wider_history_collection.history_orders_window =
            mt5bridge::ObservationWindow{0, 3000};
        require(history_coordinator.refresh(wider_history_collection).sample.has_value(),
                "wider history refresh did not produce a sample");
        require(!mt5bridge::reconciliation_descriptor_matches_graph(
                    covered_history_descriptor, history_coordinator.graph()),
                "history ticket appearing after a wider refresh was falsely attributed");
        const auto descriptor = descriptor_for(key, coordinator.graph());

        MemoryStore store;
        mt5bridge::OperationJournal journal(store);

        require(!journal.create(key, {}).accepted(),
                "empty request payload was accepted");
        const auto created = journal.create(key, {0x01, 0x02, 0x03});
        require(created.accepted() && created.record->journal_state ==
                                       mt5bridge::JournalState::created &&
                    created.record->operation_state == mt5bridge::OperationState::queued &&
                    created.record->revision == 1,
                "operation intent was not durably created");
        require(journal.create(key, {0x09}).status ==
                    mt5bridge::JournalMutationStatus::duplicate_operation,
                "duplicate operation id was accepted");
        require(journal.transition_journal(key, mt5bridge::JournalState::prechecked).status ==
                    mt5bridge::JournalMutationStatus::invalid_transition,
                "journal precheck bypassed the operation lifecycle");
        prepare_dispatch_intent(journal, key);
        FakeLease descriptor_lease;
        descriptor_lease.owned_account = key.account;
        descriptor_lease.token = 76;
        descriptor_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier descriptor_barrier(
            journal, coordinator.graph(), consistency_request);
        require(descriptor_barrier
                    .admit(key, ready_request(key.account, *fresh_consistency.proof),
                           descriptor_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "dispatch barrier opened without a durable reconciliation descriptor");
        require(journal.persist_reconciliation_descriptor(key, descriptor).accepted(),
                "reconciliation descriptor was not durably persisted");
        require(journal.persist_reconciliation_descriptor(key, descriptor).status ==
                    mt5bridge::JournalMutationStatus::invalid_transition,
                "durable reconciliation descriptor was replaced");
        require(journal.transition_operation(key, mt5bridge::OperationState::submitting)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "submitting bypassed the dispatch barrier");

        const auto scope_key = mt5bridge::OperationKey{account(), 9, 13};
        MemoryStore scope_store;
        mt5bridge::OperationJournal scope_journal(scope_store);
        require(scope_journal.create(scope_key, {0x05}).accepted(),
                "scope-bound proof setup create failed");
        prepare_for_admission(
            scope_journal, scope_key,
            descriptor_for(scope_key, positions_coordinator.graph(),
                           mt5bridge::require_position(20)));
        FakeLease scope_lease;
        scope_lease.owned_account = scope_key.account;
        scope_lease.token = 79;
        scope_lease.held = true;
        auto invalid_scope = consistency_request;
        invalid_scope.require_active_orders = false;
        invalid_scope.require_positions = false;
        mt5bridge::DispatchAdmissionBarrier invalid_scope_barrier(
            scope_journal, positions_coordinator.graph(), invalid_scope);
        require(invalid_scope_barrier
                    .admit(scope_key,
                          ready_request(scope_key.account, *positions_consistency.proof),
                          scope_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_request,
                "invalid admission scope was reported as an environment mismatch");
        mt5bridge::DispatchAdmissionBarrier full_scope_barrier(
            scope_journal, positions_coordinator.graph(), consistency_request);
        require(full_scope_barrier
                    .admit(scope_key,
                          ready_request(scope_key.account, *positions_consistency.proof),
                          scope_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "positions-only proof opened a full-scope admission barrier");
        mt5bridge::DispatchAdmissionBarrier positions_scope_barrier(
            scope_journal, positions_coordinator.graph(), positions_scope);
        require(positions_scope_barrier
                    .admit(scope_key,
                          ready_request(scope_key.account, *positions_consistency.proof),
                          scope_lease)
                    .admitted(),
                "proof matching the configured scope was rejected");

        const auto unknown_key = mt5bridge::OperationKey{account(), 10, 14};
        MemoryStore unknown_store;
        mt5bridge::OperationJournal unknown_journal(unknown_store);
        require(unknown_journal.create(unknown_key, {0x06}).accepted(),
                "unknown-ticket operation setup failed");
        prepare_dispatch_intent(unknown_journal, unknown_key);
        const auto unknown_predicate = mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::active_order_present,
            std::nullopt, false, mt5bridge::ReconciliationTransition::absent_to_present,
            9001);
        require(unknown_predicate.ticket == 0 && unknown_predicate.correlation_id == 9001,
                "unknown-ticket predicate lost its client correlation");
        require(unknown_journal
                    .persist_reconciliation_descriptor(
                        unknown_key,
                        {unknown_key.account,
                         mt5bridge::capture_reconciliation_baseline(coordinator.graph()),
                         {unknown_predicate}, mt5bridge::OperationState::filled})
                    .accepted(),
                "unknown-ticket descriptor was not durably persisted");
        require(unknown_journal
                    .transition_journal(unknown_key, mt5bridge::JournalState::dispatching, 81)
                    .accepted(),
                "unknown-ticket operation could not cross dispatching");
        require(unknown_journal
                    .transition_operation(unknown_key, mt5bridge::OperationState::submitting)
                    .accepted(),
                "unknown-ticket operation could not enter submitting");
        require(unknown_journal.persist_result(unknown_key, {0xAA}).status ==
                    mt5bridge::JournalMutationStatus::invalid_transition,
                "public result persistence bypassed unknown-ticket bindings");
        mt5bridge::ReconciliationRequest unknown_request;
        unknown_request.baseline = mt5bridge::capture_reconciliation_baseline(coordinator.graph());
        unknown_request.predicates = {unknown_predicate};
        const auto unknown_result = mt5bridge::ReconciliationEngine::evaluate(
            coordinator.graph(), unknown_request);
        require(unknown_result.outcome == mt5bridge::ReconciliationOutcome::pending &&
                    unknown_result.reason == mt5bridge::ReconciliationReason::unresolved_operation,
                "unknown broker ticket was treated as confirmed evidence");

        const auto baseline_present_key = mt5bridge::OperationKey{account(), 16, 20};
        MemoryStore baseline_present_store;
        mt5bridge::OperationJournal baseline_present_journal(baseline_present_store);
        require(baseline_present_journal.create(baseline_present_key, {0x07}).accepted(),
                "baseline-present mismatch setup failed");
        prepare_for_admission(
            baseline_present_journal, baseline_present_key,
            descriptor_for(baseline_present_key, coordinator.graph(),
                           mt5bridge::require_active_order(20)));
        FakeLease baseline_present_lease;
        baseline_present_lease.owned_account = baseline_present_key.account;
        baseline_present_lease.token = 80;
        baseline_present_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier baseline_present_barrier(
            baseline_present_journal, coordinator.graph(), consistency_request);
        require(baseline_present_barrier
                    .admit(baseline_present_key,
                          ready_request(baseline_present_key.account,
                                        *fresh_consistency.proof),
                          baseline_present_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "pre-existing ticket was accepted as an absent-to-present effect");

        const auto baseline_absent_key = mt5bridge::OperationKey{account(), 17, 21};
        MemoryStore baseline_absent_store;
        mt5bridge::OperationJournal baseline_absent_journal(baseline_absent_store);
        require(baseline_absent_journal.create(baseline_absent_key, {0x08}).accepted(),
                "baseline-absent mismatch setup failed");
        prepare_for_admission(
            baseline_absent_journal, baseline_absent_key,
            descriptor_for(baseline_absent_key, coordinator.graph(),
                           mt5bridge::require_active_order_absent(999)));
        FakeLease baseline_absent_lease;
        baseline_absent_lease.owned_account = baseline_absent_key.account;
        baseline_absent_lease.token = 81;
        baseline_absent_lease.held = true;
        mt5bridge::DispatchAdmissionBarrier baseline_absent_barrier(
            baseline_absent_journal, coordinator.graph(), consistency_request);
        require(baseline_absent_barrier
                    .admit(baseline_absent_key,
                          ready_request(baseline_absent_key.account,
                                        *fresh_consistency.proof),
                          baseline_absent_lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "already-absent ticket was accepted as a present-to-absent effect");

        mt5bridge::DispatchAdmissionBarrier barrier(journal, coordinator.graph(),
                                                    consistency_request);
        FakeLease lease;
        lease.owned_account = key.account;
        lease.token = 77;

        mt5bridge::DispatchAdmissionRequest not_ready;
        not_ready.current_account = key.account;
        require(barrier.admit(key, not_ready, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "unconfirmed environment opened the barrier");
        require(journal.find(key)->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed environment check mutated the journal");
        require(barrier
                    .admit(key, ready_request(key.account, *initial_consistency.proof), lease)
                    .status == mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "stale environment proof was replayed after a graph mutation");

        auto foreign_account = ready_request(key.account, *fresh_consistency.proof);
        foreign_account.current_account = account(43);
        require(barrier.admit(key, foreign_account, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::account_mismatch,
                "account mismatch opened the barrier");

        auto blocked = ready_request(key.account, *fresh_consistency.proof);
        blocked.unresolved_operation = true;
        require(barrier.admit(key, blocked, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::unresolved_operation,
                "unresolved operation did not block admission");
        blocked.unresolved_operation = false;
        blocked.event_gap = true;
        require(barrier.admit(key, blocked, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::event_gap,
                "event gap did not block admission");

        lease.held = false;
        const auto missing_lease =
            barrier.admit(key, ready_request(key.account, *fresh_consistency.proof), lease);
        require(missing_lease.status == mt5bridge::DispatchAdmissionStatus::lease_not_held,
                "missing writer lease opened the barrier");
        lease.held = true;
        lease.token = 0;
        require(barrier.admit(key, ready_request(key.account, *fresh_consistency.proof), lease)
                        .status ==
                    mt5bridge::DispatchAdmissionStatus::lease_not_held,
                "zero fencing token opened the barrier");
        lease.token = 77;

        store.fail_next = true;
        require(barrier.admit(key, ready_request(key.account, *fresh_consistency.proof), lease)
                        .status ==
                    mt5bridge::DispatchAdmissionStatus::not_durable,
                "failed dispatching commit was reported as admitted");
        require(journal.find(key)->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed dispatching commit changed the owner cache");
        require(store.load(key).found() && store.load(key).record->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed dispatching commit changed durable state");

        auto admitted = barrier.admit(
            key, ready_request(key.account, *fresh_consistency.proof), lease);
        require(admitted.admitted() && admitted.permit->fencing_token() == 77 &&
                    journal.find(key)->journal_state == mt5bridge::JournalState::dispatching,
                "valid admission did not commit the non-resendable barrier");
        auto consumed_permit = std::move(*admitted.permit);
        require(consumed_permit.valid() && !admitted.permit->valid(),
                "moving a dispatch permit left a second usable capability");
        require(barrier.admit(key, ready_request(key.account, *fresh_consistency.proof), lease)
                        .status ==
                    mt5bridge::DispatchAdmissionStatus::invalid_state,
                "dispatching operation was admitted a second time");

        MemoryStore recovered_store = store;
        mt5bridge::OperationJournal recovered_journal(recovered_store);
        const auto recovered = recovered_journal.recover(key);
        require(recovered.accepted() && recovered.record->journal_state ==
                                       mt5bridge::JournalState::dispatching &&
                    recovered.record->fencing_token == 77,
                "dispatching barrier was not recoverable after restart");
        mt5bridge::DispatchAdmissionBarrier recovered_barrier(recovered_journal,
                                                               coordinator.graph(),
                                                               consistency_request);
        require(recovered_barrier
                    .admit(key, ready_request(key.account, *fresh_consistency.proof), lease)
                    .status == mt5bridge::DispatchAdmissionStatus::invalid_state,
                "recovered dispatching operation became resendable");
        require(recovered_journal
                    .transition_journal(key, mt5bridge::JournalState::reconciling)
                    .accepted(),
                "crash recovery could not enter journal reconciliation");
        require(recovered_journal.transition_operation(
                    key, mt5bridge::OperationState::reconciling)
                    .accepted(),
                "crash recovery could not enter operation reconciliation");
        require(recovered_journal
                    .transition_operation(key, mt5bridge::OperationState::failed)
                    .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "reconciling operation was incorrectly downgraded to failed");

        const auto entered_backend =
            journal.transition_operation(key, mt5bridge::OperationState::submitting);
        require(entered_backend.accepted(),
                "operation could not enter the backend after dispatching");

        require(journal.transition_journal(key, mt5bridge::JournalState::result_persisted)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "generic result_persisted transition bypassed result payload persistence");
        require(journal.persist_result(key, {}).status ==
                    mt5bridge::JournalMutationStatus::invalid_transition,
                "empty backend result was persisted");
        require(journal.transition_operation(key, mt5bridge::OperationState::accepted)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "accepted operation state bypassed result persistence");
        require(journal.transition_operation(key, mt5bridge::OperationState::rejected)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "rejected operation state bypassed result persistence");
        const std::vector<std::uint8_t> result_payload{0xA0, 0x01};
        require(journal.persist_result(key, result_payload).accepted(),
                "backend result payload was not persisted atomically");
        require(journal.transition_operation(key, mt5bridge::OperationState::accepted)
                        .accepted(),
                "accepted operation state was rejected after result persistence");
        mt5bridge::OperationJournal result_recovered_journal(store);
        const auto result_recovered = result_recovered_journal.recover(key);
        require(result_recovered.accepted() && result_recovered.record->operation_state ==
                                             mt5bridge::OperationState::accepted &&
                    result_recovered.record->result_payload == result_payload,
                "persisted backend result was not preserved through recovery");
        require(journal.transition_operation(key, mt5bridge::OperationState::reconciling)
                        .accepted(),
                "result-bearing operation did not enter reconciliation");
        require(journal.transition_journal(key, mt5bridge::JournalState::reconciling)
                        .accepted(),
                "result journal state did not enter reconciliation");

        auto malformed_store = store;
        malformed_store.durable[key].fencing_token = 0;
        mt5bridge::OperationJournal malformed_journal(malformed_store);
        require(malformed_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "malformed durable fencing record was recovered");
        auto missing_descriptor_store = store;
        missing_descriptor_store.durable[key].reconciliation_descriptor.reset();
        mt5bridge::OperationJournal missing_descriptor_journal(
            missing_descriptor_store);
        require(missing_descriptor_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "post-dispatch record without a reconciliation descriptor was recovered");
        auto malformed_pair_store = store;
        malformed_pair_store.durable[key].operation_state =
            mt5bridge::OperationState::queued;
        mt5bridge::OperationJournal malformed_pair_journal(malformed_pair_store);
        require(malformed_pair_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "incompatible journal/operation state pair was recovered");
        auto malformed_reconciling_store = store;
        malformed_reconciling_store.durable[key].journal_state =
            mt5bridge::JournalState::reconciling;
        malformed_reconciling_store.durable[key].operation_state =
            mt5bridge::OperationState::failed;
        mt5bridge::OperationJournal malformed_reconciling_journal(
            malformed_reconciling_store);
        require(malformed_reconciling_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "reconciling plus failed state pair was recovered");
        auto malformed_accepted_store = store;
        malformed_accepted_store.durable[key].operation_state =
            mt5bridge::OperationState::accepted;
        malformed_accepted_store.durable[key].result_payload.clear();
        mt5bridge::OperationJournal malformed_accepted_journal(malformed_accepted_store);
        require(malformed_accepted_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "accepted operation without a durable result was recovered");

        const auto stale_key = mt5bridge::OperationKey{account(), 8, 12};
        MemoryStore stale_store;
        mt5bridge::OperationJournal owner_a(stale_store);
        require(owner_a.create(stale_key, {0x04}).accepted(),
                "stale-writer setup create failed");
        prepare_for_admission(owner_a, stale_key, descriptor);
        mt5bridge::OperationJournal owner_b(stale_store);
        require(owner_b.recover(stale_key).accepted(),
                "stale-writer setup recovery failed");
        mt5bridge::DispatchAdmissionBarrier barrier_a(owner_a, coordinator.graph(),
                                                      consistency_request);
        mt5bridge::DispatchAdmissionBarrier barrier_b(owner_b, coordinator.graph(),
                                                      consistency_request);
        const auto admitted_a = barrier_a.admit(
            stale_key, ready_request(stale_key.account, *fresh_consistency.proof), lease);
        require(admitted_a.admitted(), "first stale-writer owner was not admitted");
        lease.token = 78;
        const auto stale_result = barrier_b.admit(
            stale_key, ready_request(stale_key.account, *fresh_consistency.proof), lease);
        require(stale_result.status == mt5bridge::DispatchAdmissionStatus::conflict &&
                    stale_store.load(stale_key).found() &&
                        stale_store.load(stale_key).record->fencing_token == 77,
                "stale owner overwrote a newer dispatching record");
        require(owner_b.recover(stale_key).record->journal_state ==
                    mt5bridge::JournalState::dispatching,
                "stale owner did not observe the durable winner after conflict");

        require(mt5bridge::OperationJournal::can_transition_journal(
                    mt5bridge::JournalState::dispatching,
                    mt5bridge::JournalState::reconciling),
                "dispatching to reconciling recovery edge is missing");
        require(!mt5bridge::OperationJournal::can_transition_journal(
                    mt5bridge::JournalState::reconciling,
                    mt5bridge::JournalState::dispatching),
                "reconciling state became resendable");

        const auto recovery_key = mt5bridge::OperationKey{account(), 14, 15};
        MemoryStore recovery_store;
        mt5bridge::OperationJournal recovery_journal(recovery_store);
        require(recovery_journal.create(recovery_key, {0x0C}).accepted(),
                "recovery-status setup create failed");
        recovery_store.load_status = mt5bridge::StoreLoadStatus::invalid_record;
        require(recovery_journal.recover(recovery_key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "malformed load was reported as missing during recovery");
        recovery_store.load_status = mt5bridge::StoreLoadStatus::io_error;
        require(recovery_journal.recover(recovery_key).status ==
                    mt5bridge::JournalMutationStatus::storage_error,
                "I/O load failure was reported as missing during recovery");
        recovery_store.load_status = mt5bridge::StoreLoadStatus::found;
        require(recovery_journal.recover_all().accepted(),
                "complete recovery scan was rejected");
        recovery_store.scan_status = mt5bridge::StoreScanStatus::invalid_record;
        require(recovery_journal.recover_all().status ==
                    mt5bridge::JournalMutationStatus::invalid_record &&
                    recovery_journal.find(recovery_key),
                "failed recovery scan partially replaced the owner cache");

        std::cout << "dispatch journal checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "dispatch journal checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

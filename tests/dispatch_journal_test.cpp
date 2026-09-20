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

    std::optional<mt5bridge::OperationRecord> load(
        const mt5bridge::OperationKey &key) const override {
        const auto it = durable.find(key);
        return it == durable.end() ? std::nullopt
                                   : std::optional<mt5bridge::OperationRecord>(it->second);
    }

    bool fail_next = false;
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

mt5bridge::ObservationBatch positions_only_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::positions;
    return batch;
}

mt5bridge::DispatchAdmissionRequest ready_request(
    const mt5bridge::AccountKey &key,
    const mt5bridge::EnvironmentConsistencyProof &proof) {
    mt5bridge::DispatchAdmissionRequest request;
    request.current_account = key;
    request.environment_proof = proof;
    return request;
}

void prepare_for_admission(mt5bridge::OperationJournal &journal,
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
        prepare_for_admission(journal, key);
        require(journal.transition_operation(key, mt5bridge::OperationState::submitting)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "submitting bypassed the dispatch barrier");

        const auto scope_key = mt5bridge::OperationKey{account(), 9, 13};
        MemoryStore scope_store;
        mt5bridge::OperationJournal scope_journal(scope_store);
        require(scope_journal.create(scope_key, {0x05}).accepted(),
                "scope-bound proof setup create failed");
        prepare_for_admission(scope_journal, scope_key);
        FakeLease scope_lease;
        scope_lease.owned_account = scope_key.account;
        scope_lease.token = 79;
        scope_lease.held = true;
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
        require(barrier.admit(key, ready_request(key.account, *fresh_consistency.proof), lease)
                        .status ==
                    mt5bridge::DispatchAdmissionStatus::lease_not_held,
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
        require(store.load(key)->journal_state ==
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
        auto malformed_pair_store = store;
        malformed_pair_store.durable[key].operation_state =
            mt5bridge::OperationState::queued;
        mt5bridge::OperationJournal malformed_pair_journal(malformed_pair_store);
        require(malformed_pair_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "incompatible journal/operation state pair was recovered");
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
        prepare_for_admission(owner_a, stale_key);
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
                    stale_store.load(stale_key)->fencing_token == 77,
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

        std::cout << "dispatch journal checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "dispatch journal checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

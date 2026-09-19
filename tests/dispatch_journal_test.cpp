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
    bool commit(const mt5bridge::OperationRecord &record) override {
        if (fail_next) {
            fail_next = false;
            return false;
        }
        durable[record.key] = record;
        commits.push_back(record);
        return true;
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

mt5bridge::DispatchAdmissionRequest ready_request(const mt5bridge::AccountKey &key) {
    mt5bridge::DispatchAdmissionRequest request;
    request.current_account = key;
    request.environment.account = key;
    request.environment.state = mt5bridge::EnvironmentConsistencyState::consistent;
    request.environment.observation_count = 2;
    return request;
}

} // namespace

/// \brief Runs durable journal and dispatch-admission contract checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto key = mt5bridge::OperationKey{account(), 7, 11};
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
        require(journal.transition_operation(key, mt5bridge::OperationState::submitting)
                        .status == mt5bridge::JournalMutationStatus::invalid_transition,
                "submitting bypassed the dispatch barrier");

        mt5bridge::DispatchAdmissionBarrier barrier(journal);
        FakeLease lease;
        lease.owned_account = key.account;
        lease.token = 77;

        auto not_ready = ready_request(key.account);
        not_ready.environment.state =
            mt5bridge::EnvironmentConsistencyState::awaiting_confirmation;
        require(barrier.admit(key, not_ready, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "unconfirmed environment opened the barrier");
        require(journal.find(key)->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed environment check mutated the journal");
        not_ready.environment.state = mt5bridge::EnvironmentConsistencyState::consistent;
        not_ready.environment.observation_count = 1;
        require(barrier.admit(key, not_ready, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::environment_not_ready,
                "single observation was treated as a stable environment");

        auto foreign_account = ready_request(key.account);
        foreign_account.current_account = account(43);
        require(barrier.admit(key, foreign_account, lease).status ==
                    mt5bridge::DispatchAdmissionStatus::account_mismatch,
                "account mismatch opened the barrier");

        auto blocked = ready_request(key.account);
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
        require(barrier.admit(key, ready_request(key.account), lease).status ==
                    mt5bridge::DispatchAdmissionStatus::lease_not_held,
                "missing writer lease opened the barrier");
        lease.held = true;
        lease.token = 0;
        require(barrier.admit(key, ready_request(key.account), lease).status ==
                    mt5bridge::DispatchAdmissionStatus::lease_not_held,
                "zero fencing token opened the barrier");
        lease.token = 77;

        store.fail_next = true;
        require(barrier.admit(key, ready_request(key.account), lease).status ==
                    mt5bridge::DispatchAdmissionStatus::not_durable,
                "failed dispatching commit was reported as admitted");
        require(journal.find(key)->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed dispatching commit changed the owner cache");
        require(store.load(key)->journal_state ==
                    mt5bridge::JournalState::dispatch_intent_persisted,
                "failed dispatching commit changed durable state");

        const auto admitted = barrier.admit(key, ready_request(key.account), lease);
        require(admitted.admitted() && admitted.permit->fencing_token == 77 &&
                    journal.find(key)->journal_state == mt5bridge::JournalState::dispatching,
                "valid admission did not commit the non-resendable barrier");
        require(barrier.admit(key, ready_request(key.account), lease).status ==
                    mt5bridge::DispatchAdmissionStatus::invalid_state,
                "dispatching operation was admitted a second time");

        const auto entered_backend =
            journal.transition_operation(key, mt5bridge::OperationState::submitting);
        require(entered_backend.accepted(),
                "operation could not enter the backend after dispatching");

        MemoryStore recovered_store = store;
        mt5bridge::OperationJournal recovered_journal(recovered_store);
        const auto recovered = recovered_journal.recover(key);
        require(recovered.accepted() && recovered.record->journal_state ==
                                       mt5bridge::JournalState::dispatching &&
                    recovered.record->fencing_token == 77,
                "dispatching barrier was not recoverable after restart");
        mt5bridge::DispatchAdmissionBarrier recovered_barrier(recovered_journal);
        require(recovered_barrier.admit(key, ready_request(key.account), lease).status ==
                    mt5bridge::DispatchAdmissionStatus::invalid_state,
                "recovered dispatching operation became resendable");

        auto malformed_store = store;
        malformed_store.durable[key].fencing_token = 0;
        mt5bridge::OperationJournal malformed_journal(malformed_store);
        require(malformed_journal.recover(key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "malformed durable fencing record was recovered");

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

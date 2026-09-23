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

void enter_dispatching(mt5bridge::OperationJournal &journal,
                       const mt5bridge::OperationKey &key) {
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
        enter_dispatching(journal, key);

        const auto recovered = mt5bridge::OperationRecoveryCoordinator::recover(journal);
        require(recovered.accepted() && recovered.operations.size() == 1 &&
                    recovered.operations.front().non_resendable() &&
                    recovered.operations.front().action ==
                        mt5bridge::OperationRecoveryAction::reconcile_only,
                "dispatching recovery became resendable");

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

        const auto unresolved_key = mt5bridge::OperationKey{account(), 8, 12};
        require(journal.create(unresolved_key, {0x02}).accepted(),
                "unresolved operation create failed");
        enter_dispatching(journal, unresolved_key);
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
        mt5bridge::OperationReconciliationWorker unresolved_worker(
            journal, unresolved_key, unresolved_coordinator, unresolved_collection,
            unresolved_request);
        const auto not_observed = unresolved_worker.step(false, true);
        require(not_observed.status ==
                    mt5bridge::OperationReconciliationStatus::not_observed &&
                    not_observed.record &&
                    not_observed.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "not-observed evidence became a terminal lifecycle state");

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

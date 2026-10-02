/// \file managed_trade_owner_test.cpp
/// \brief Exercises the private managed-trade owner-loop composition seam.

#include "managed_trade_owner.hpp"

#include <mt5bridge/dispatch/operation_recovery.hpp>
#include <mt5bridge.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
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

private:
    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> records;
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

mt5bridge::ObservationBatch observation_batch(const mt5bridge::AccountKey &key,
                                              std::uint64_t ticket = 20) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    Mt5OrderSnapshot order{};
    order.ticket = ticket;
    order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    batch.active_orders.push_back(order);
    return batch;
}

class FakeLease final : public mt5bridge::SingleWriterLease {
public:
    explicit FakeLease(mt5bridge::AccountKey owned) : owned_account(std::move(owned)) {}

    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &requested) const override {
        if (requested != owned_account)
            return std::nullopt;
        return token;
    }

    mt5bridge::AccountKey owned_account;
    std::uint64_t token = 77;
};

class FakeAccountProbe final : public mt5bridge::runtime::CurrentAccountProbe {
public:
    explicit FakeAccountProbe(std::vector<std::optional<mt5bridge::AccountKey>> readings)
        : readings_(std::move(readings)) {}

    std::optional<mt5bridge::AccountKey> current_account() override {
        if (next_ == readings_.size())
            return std::nullopt;
        return readings_[next_++];
    }

private:
    std::vector<std::optional<mt5bridge::AccountKey>> readings_;
    std::size_t next_ = 0;
};

class FakeTransport final : public mt5bridge::runtime::DispatchTransport {
public:
    mt5bridge::runtime::BackendCallResult submit_once(
        const mt5bridge::OperationRecord &) override {
        ++calls;
        return next;
    }

    mt5bridge::runtime::BackendCallResult next{
        mt5bridge::runtime::BackendCallStatus::broker_result,
        mt5bridge::runtime::BrokerResultDisposition::accepted,
        10009,
        {0xA0, 0x01}};
    std::size_t calls = 0;
};

} // namespace

int main() {
    try {
        const auto operation_account = account();
        auto provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                observation_batch(operation_account), observation_batch(operation_account),
                observation_batch(operation_account, 999),
                observation_batch(operation_account, 999),
                observation_batch(operation_account, 999)});
        mt5bridge::ObservationCoordinator coordinator(*provider, operation_account);
        mt5bridge::ObservationCollectionRequest collection;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample,
                "owner-loop observation setup failed");

        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, consistency_request);
        require(consistency.consistent() && consistency.proof,
                "owner-loop environment proof setup failed");

        MemoryStore store;
        mt5bridge::OperationJournal journal(store);
        mt5bridge::EnvironmentConsistencyRequest scope;
        mt5bridge::DispatchAdmissionBarrier admission(journal, coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend backend;
        FakeLease lease{operation_account};
        FakeTransport transport;
        FakeAccountProbe account_probe({operation_account, operation_account});

        mt5bridge::managed_trade::ManagedTradeState initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &initial, mt5bridge::managed_trade::TradeId{7}, 5, 5, 4),
                "owner-loop state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner owner(
            initial, operation_account, journal, admission, backend, account_probe, lease,
            transport);

        mt5bridge::dispatch::ManagedTradeIntent intent{
            {0x01, 0x02},
            {operation_account, coordinator.capture_baseline(),
             {mt5bridge::require_active_order(999)}, mt5bridge::OperationState::filled}};
        const auto prepared = owner.prepare_open(5, std::move(intent));
        require(prepared.status == mt5bridge::dispatch::OwnerStepStatus::prepared &&
                    prepared.key && prepared.record &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::dispatching &&
                    prepared.record->journal_state ==
                        mt5bridge::JournalState::dispatch_intent_persisted,
                "owner loop did not durably prepare an open slice");

        mt5bridge::DispatchAdmissionRequest blocked;
        blocked.current_account = {"Other-Terminal", 9};
        const auto blocked_result = owner.execute_pending(blocked);
        require(blocked_result.status ==
                    mt5bridge::dispatch::OwnerStepStatus::admission_rejected &&
                    blocked_result.admission_status ==
                        mt5bridge::DispatchAdmissionStatus::account_mismatch &&
                    transport.calls == 0 &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::dispatching,
                "owner loop reached transport without an admitted permit");

        mt5bridge::DispatchAdmissionRequest ready;
        ready.current_account = operation_account;
        ready.environment_proof = consistency.proof;
        const auto executed = owner.execute_pending(ready);
        require(executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    executed.execution_status ==
                        mt5bridge::runtime::OneShotExecutionStatus::completed &&
                    executed.retcode == 10009 && transport.calls == 1 &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting,
                "owner loop did not require durable permit before one-shot execution");

        const auto accepted_record =
            executed.key ? journal.find(*executed.key) : std::nullopt;
        require(executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    accepted_record &&
                    (accepted_record->journal_state ==
                         mt5bridge::JournalState::result_persisted ||
                     accepted_record->journal_state ==
                         mt5bridge::JournalState::reconciling) &&
                    accepted_record->operation_state ==
                        mt5bridge::OperationState::accepted &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    owner.state().open_volume == 0,
                "owner loop allowed unproven evidence to settle accepted operation");

        mt5bridge::OperationReconciliationWorker settlement_worker(
            journal, *executed.key, coordinator, collection);
        const auto settled = owner.settle_reconciliation(settlement_worker);
        require(settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    settled.record &&
                    settled.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    settled.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::filled &&
                    owner.state().open_volume == 5,
                "owner loop did not settle through authoritative observation");

        require(settlement_worker.last_cycle() &&
                    settlement_worker.last_cycle()->refresh.sample,
                "settlement worker did not retain observation provenance");
        const auto stabilized = coordinator.refresh(collection);
        require(stabilized.sample.has_value(),
                "post-settlement observation refresh failed");
        const auto post_settlement_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*settlement_worker.last_cycle()->refresh.sample,
                 *stabilized.sample},
                consistency_request);
        require(post_settlement_consistency.consistent() &&
                    post_settlement_consistency.proof,
                "post-settlement environment proof setup failed");
        ready.environment_proof = post_settlement_consistency.proof;

        mt5bridge::managed_trade::ManagedTradeState uncertain_initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &uncertain_initial, mt5bridge::managed_trade::TradeId{8}, 5, 5, 4),
                "uncertain owner state initialization failed");
        FakeTransport uncertain_transport;
        uncertain_transport.next = {
            mt5bridge::runtime::BackendCallStatus::transport_failure,
            mt5bridge::runtime::BrokerResultDisposition::reconciling, 10018, {}};
        FakeAccountProbe uncertain_probe({operation_account, operation_account});
        mt5bridge::dispatch::ManagedTradeOwner uncertain_owner(
            uncertain_initial, operation_account, journal, admission, backend,
            uncertain_probe, lease, uncertain_transport);
        mt5bridge::dispatch::ManagedTradeIntent uncertain_intent{
            {0x03},
            {operation_account, coordinator.capture_baseline(),
             {mt5bridge::require_active_order(998)},
             mt5bridge::OperationState::filled}};
        require(uncertain_owner.prepare_open(5, std::move(uncertain_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "uncertain owner did not prepare intent");
        const auto uncertain_result = uncertain_owner.execute_pending(ready);
        require(uncertain_result.status ==
                    mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                    uncertain_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    uncertain_result.record &&
                    uncertain_result.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    uncertain_result.record->operation_state ==
                        mt5bridge::OperationState::submitting &&
                    mt5bridge::OperationRecoveryCoordinator::classify(
                        *uncertain_result.record) ==
                        mt5bridge::OperationRecoveryAction::reconcile_only &&
                    uncertain_transport.calls == 1,
                "transport uncertainty did not become durable reconcile-only state");

        mt5bridge::OperationReconciliationWorker pending_worker(
            journal, *uncertain_result.key, coordinator, collection);
        const auto pending = uncertain_owner.settle_reconciliation(pending_worker);
        require(pending.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    pending.record &&
                    pending.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    pending.record->operation_state ==
                        mt5bridge::OperationState::reconciling &&
                    uncertain_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    uncertain_owner.state().open_volume == 0,
                "pending reconciliation changed managed state without proof");
        require(pending_worker.last_cycle() &&
                    pending_worker.last_cycle()->refresh.sample,
                "pending worker did not retain observation provenance");
        const auto post_pending_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*stabilized.sample,
                 *pending_worker.last_cycle()->refresh.sample},
                consistency_request);
        require(post_pending_consistency.consistent() &&
                    post_pending_consistency.proof,
                "post-pending environment proof setup failed");
        ready.environment_proof = post_pending_consistency.proof;
        const auto retry = uncertain_owner.execute_pending(ready);
        require(retry.status == mt5bridge::dispatch::OwnerStepStatus::invalid_state &&
                    uncertain_transport.calls == 1,
                "unresolved owner state enabled a second backend call");

        mt5bridge::managed_trade::ManagedTradeState pre_transport_initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &pre_transport_initial, mt5bridge::managed_trade::TradeId{9}, 5, 5, 4),
                "pre-transport owner state initialization failed");
        FakeTransport pre_transport;
        FakeAccountProbe pre_transport_probe({operation_account,
                                               mt5bridge::AccountKey{"Other-Terminal", 9}});
        mt5bridge::dispatch::ManagedTradeOwner pre_transport_owner(
            pre_transport_initial, operation_account, journal, admission, backend,
            pre_transport_probe, lease, pre_transport);
        mt5bridge::dispatch::ManagedTradeIntent pre_transport_intent{
            {0x04},
            {operation_account, coordinator.capture_baseline(),
             {mt5bridge::require_active_order(997)},
             mt5bridge::OperationState::filled}};
        require(pre_transport_owner.prepare_open(5, std::move(pre_transport_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "pre-transport owner did not prepare intent");
        const auto pre_transport_result = pre_transport_owner.execute_pending(ready);
        require(pre_transport_result.status ==
                    mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                    pre_transport_result.execution_status ==
                        mt5bridge::runtime::OneShotExecutionStatus::account_mismatch &&
                    pre_transport_result.record &&
                    pre_transport_result.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    pre_transport_result.record->operation_state ==
                        mt5bridge::OperationState::submitting &&
                    mt5bridge::OperationRecoveryCoordinator::classify(
                        *pre_transport_result.record) ==
                        mt5bridge::OperationRecoveryAction::reconcile_only &&
                    pre_transport_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    pre_transport.calls == 0,
                "post-barrier account failure reached transport or remained resendable");

        mt5bridge::OperationJournal recovered_journal(store);
        const auto recovered =
            mt5bridge::OperationRecoveryCoordinator::recover(recovered_journal);
        const auto recovered_entry =
            pre_transport_result.key
                ? recovered_journal.find(*pre_transport_result.key)
                : std::nullopt;
        const auto recovered_classification =
            recovered_entry
                ? mt5bridge::OperationRecoveryCoordinator::classify(*recovered_entry)
                : mt5bridge::OperationRecoveryAction::terminal;
        require(recovered.accepted() && recovered_entry &&
                    recovered_entry->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    recovered_entry->operation_state ==
                        mt5bridge::OperationState::submitting &&
                    recovered_classification ==
                        mt5bridge::OperationRecoveryAction::reconcile_only,
                "post-barrier record did not survive restart as reconcile-only");

        const auto pre_transport_retry = pre_transport_owner.execute_pending(ready);
        require(pre_transport_retry.status ==
                    mt5bridge::dispatch::OwnerStepStatus::invalid_state &&
                    pre_transport.calls == 0,
                "post-barrier failure enabled a second send");
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

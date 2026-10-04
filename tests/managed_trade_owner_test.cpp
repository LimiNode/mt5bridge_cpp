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
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_default_constructible_v<mt5bridge::JournalRecoveryResult>);
static_assert(!std::is_constructible_v<mt5bridge::JournalRecoveryResult,
                                      mt5bridge::JournalMutationStatus,
                                      std::vector<mt5bridge::OperationRecord>, bool>);
static_assert(std::is_same_v<
              decltype(std::declval<const mt5bridge::JournalRecoveryResult &>().records()),
              const std::vector<mt5bridge::OperationRecord> &>);

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

mt5bridge::ObservationBatch history_deals_batch(
    const mt5bridge::AccountKey &key, mt5bridge::ObservationWindow window,
    std::vector<Mt5DealSnapshot> deals) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions |
                             mt5bridge::ObservationDomain::history_deals;
    batch.history_deals_window = window;
    batch.history_deals = std::move(deals);
    return batch;
}

Mt5DealSnapshot history_deal(std::uint64_t ticket, std::uint64_t order_ticket,
                             std::uint64_t position_id, double volume,
                             std::uint32_t entry = 0) {
    Mt5DealSnapshot deal{};
    deal.ticket = ticket;
    deal.order_ticket = order_ticket;
    deal.position_id = position_id;
    deal.entry = entry;
    deal.volume = volume;
    deal.time_msc = 1500;
    deal.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                        MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                        MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                        MT5BRIDGE_DEAL_KNOWN_ENTRY |
                        MT5BRIDGE_DEAL_KNOWN_VOLUME |
                        MT5BRIDGE_DEAL_KNOWN_TIME;
    return deal;
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
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    settled.record &&
                    settled.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    settled.record->operation_state ==
                        mt5bridge::OperationState::reconciling &&
                    owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    owner.state().open_volume == 0,
                "active-order provenance was mistaken for a full fill");

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

        auto recovered_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                observation_batch(operation_account),
                observation_batch(operation_account, 999)});
        mt5bridge::ObservationCoordinator recovered_coordinator(
            *recovered_provider, operation_account);
        require(recovered_coordinator.refresh(collection).sample.has_value(),
                "recovered graph baseline refresh failed");
        mt5bridge::OperationJournal restart_journal(store);
        require(mt5bridge::OperationRecoveryCoordinator::recover(restart_journal)
                    .accepted(),
                "recovered journal scan failed");
        mt5bridge::EnvironmentConsistencyRequest recovered_scope;
        mt5bridge::DispatchAdmissionBarrier recovered_admission(
            restart_journal, recovered_coordinator.graph(), recovered_scope);
        mt5bridge::runtime::OneShotDispatchBackend recovered_backend;
        FakeAccountProbe recovered_probe({operation_account});
        FakeTransport recovered_transport;
        mt5bridge::managed_trade::ManagedTradeState recovered_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &recovered_state, mt5bridge::managed_trade::TradeId{7}, 5, 5, 4) &&
                    recovered_state.start_open_slice(5) ==
                        mt5bridge::managed_trade::MutationStatus::applied &&
                    recovered_state.enter_submitting() ==
                        mt5bridge::managed_trade::MutationStatus::applied,
                "recovered managed state setup failed");
        mt5bridge::dispatch::ManagedTradeOwner recovered_owner(
            recovered_state, operation_account, restart_journal, recovered_admission,
            recovered_backend, recovered_probe, lease, recovered_transport);
        const auto recovered_key = mt5bridge::OperationKey{operation_account, 7, 1};
        mt5bridge::OperationReconciliationWorker recovered_worker(
            restart_journal, recovered_key, recovered_coordinator, collection);
        const auto recovered_settlement =
            recovered_owner.settle_reconciliation(recovered_worker);
        require(recovered_settlement.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    recovered_settlement.record &&
                    recovered_settlement.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    recovered_settlement.record->operation_state ==
                        mt5bridge::OperationState::reconciling &&
                    recovered_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::submitting &&
                    recovered_owner.state().open_volume == 0,
                "recovered graph provenance used the old graph revision namespace");

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

        const mt5bridge::ObservationWindow deal_window{1000, 2000};
        auto deal_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(700, 900, 901, 2.0),
                     history_deal(701, 900, 901, 3.0)})});
        mt5bridge::ObservationCoordinator deal_coordinator(
            *deal_provider, operation_account);
        mt5bridge::ObservationCollectionRequest deal_collection;
        deal_collection.history_deals_window = deal_window;
        const auto deal_first = deal_coordinator.refresh(deal_collection);
        const auto deal_second = deal_coordinator.refresh(deal_collection);
        require(deal_first.sample && deal_second.sample,
                "history-deal settlement baseline setup failed");
        mt5bridge::EnvironmentConsistencyRequest deal_consistency_request;
        deal_consistency_request.history_deals_window = deal_window;
        const auto deal_consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*deal_first.sample, *deal_second.sample}, deal_consistency_request);
        require(deal_consistency.consistent() && deal_consistency.proof,
                "history-deal environment proof setup failed");

        mt5bridge::DispatchAdmissionBarrier deal_admission(
            journal, deal_coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend deal_backend;
        FakeTransport deal_transport;
        deal_transport.next.reconciliation_bindings = {{501, 700}};
        FakeAccountProbe deal_probe({operation_account, operation_account});
        mt5bridge::managed_trade::ManagedTradeState deal_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &deal_state, mt5bridge::managed_trade::TradeId{50}, 5, 5, 4),
                "history-deal managed state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner deal_owner(
            deal_state, operation_account, journal, deal_admission, deal_backend,
            deal_probe, lease, deal_transport);
        const auto deal_predicate = mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::history_deal_present,
            std::nullopt, false, mt5bridge::ReconciliationTransition::absent_to_present,
            501, deal_window);
        mt5bridge::dispatch::ManagedTradeIntent deal_intent{
            {0x50},
            {operation_account, deal_coordinator.capture_baseline(),
             {deal_predicate}, mt5bridge::OperationState::filled}};
        const auto deal_prepared = deal_owner.prepare_open(5, std::move(deal_intent));
        require(deal_prepared.status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "history-deal owner did not prepare the open slice");
        mt5bridge::DispatchAdmissionRequest deal_ready;
        deal_ready.current_account = operation_account;
        deal_ready.environment_proof = deal_consistency.proof;
        const auto deal_executed = deal_owner.execute_pending(deal_ready);
        require(deal_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    deal_executed.key,
                "history-deal owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker deal_worker(
            journal, *deal_executed.key, deal_coordinator, deal_collection);
        const auto deal_settled = deal_owner.settle_reconciliation(deal_worker);
        require(deal_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    deal_settled.record &&
                    deal_settled.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    deal_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::filled &&
                    deal_owner.state().open_volume == 5 &&
                    deal_owner.state().pending_remainder_volume == 0,
                "fresh attributed history deals did not prove a full fill");

        auto partial_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(712, 910, 911, 99.0, 1)}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(710, 910, 911, 1.0)}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(712, 910, 911, 99.0, 1),
                     history_deal(713, 910, 911, 2.0)}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(712, 910, 911, 99.0, 1),
                     history_deal(713, 910, 911, 2.0),
                     history_deal(714, 910, 911, 1.0)})});
        mt5bridge::ObservationCoordinator partial_deal_coordinator(
            *partial_provider, operation_account);
        const auto partial_first =
            partial_deal_coordinator.refresh(deal_collection);
        const auto partial_second =
            partial_deal_coordinator.refresh(deal_collection);
        require(partial_first.sample && partial_second.sample,
                "partial history-deal baseline setup failed");
        const auto partial_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*partial_first.sample, *partial_second.sample},
                deal_consistency_request);
        require(partial_consistency.consistent() && partial_consistency.proof,
                "partial history-deal environment proof setup failed");
        mt5bridge::DispatchAdmissionBarrier partial_admission(
            journal, partial_deal_coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend partial_backend;
        FakeTransport partial_transport;
        partial_transport.next.reconciliation_bindings = {{601, 710}};
        FakeAccountProbe partial_probe({operation_account, operation_account});
        mt5bridge::managed_trade::ManagedTradeState partial_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &partial_state, mt5bridge::managed_trade::TradeId{51}, 5, 5, 4),
                "partial history-deal managed state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner partial_owner(
            partial_state, operation_account, journal, partial_admission,
            partial_backend, partial_probe, lease, partial_transport);
        const auto partial_predicate = mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::history_deal_present,
            std::nullopt, false, mt5bridge::ReconciliationTransition::absent_to_present,
            601, deal_window);
        mt5bridge::dispatch::ManagedTradeIntent partial_intent{
            {0x51},
            {operation_account, partial_deal_coordinator.capture_baseline(),
             {partial_predicate}, mt5bridge::OperationState::filled}};
        require(partial_owner.prepare_open(5, std::move(partial_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "partial history-deal owner did not prepare the open slice");
        mt5bridge::DispatchAdmissionRequest partial_ready;
        partial_ready.current_account = operation_account;
        partial_ready.environment_proof = partial_consistency.proof;
        const auto partial_executed = partial_owner.execute_pending(partial_ready);
        require(partial_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    partial_executed.key,
                "partial history-deal owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker partial_worker(
            journal, *partial_executed.key, partial_deal_coordinator, deal_collection);
        const auto partial_settled =
            partial_owner.settle_reconciliation(partial_worker);
        require(partial_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::partially_filled &&
                    partial_settled.record &&
                    partial_settled.record->operation_state ==
                        mt5bridge::OperationState::partially_filled &&
                    partial_settled.record->operation_kind ==
                        mt5bridge::OperationKind::open &&
                    partial_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled &&
                    partial_owner.state().open_volume == 2 &&
                    partial_owner.state().pending_remainder_volume == 3,
                "aggregated entry deals did not prove a partial fill");

        mt5bridge::OperationJournal partial_restart_journal(store);
        const auto partial_recovered_records = partial_restart_journal.recover_all();
        require(partial_recovered_records.accepted(),
                "partial-fill restart scan did not recover durable records");
        mt5bridge::managed_trade::ManagedTradeState partial_recovery_seed;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &partial_recovery_seed, mt5bridge::managed_trade::TradeId{51}, 5, 5,
                    4),
                "partial-fill recovery seed initialization failed");
        const auto partial_recovered =
            mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
                partial_recovery_seed, operation_account, partial_recovered_records);
        require(partial_recovered && partial_recovered->valid() &&
                    partial_recovered->slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled &&
                    partial_recovered->open_volume == 2 &&
                    partial_recovered->pending_remainder_volume == 3,
                "durable partial fill did not reconstruct managed remainder");

        MemoryStore wrong_kind_store = store;
        const auto wrong_kind_loaded = wrong_kind_store.load(*partial_executed.key);
        require(wrong_kind_loaded.found() && wrong_kind_loaded.record,
                "wrong-kind recovery fixture could not load the partial record");
        auto wrong_kind_record = *wrong_kind_loaded.record;
        wrong_kind_record.operation_kind = mt5bridge::OperationKind::close;
        ++wrong_kind_record.revision;
        require(wrong_kind_record.valid() &&
                    wrong_kind_store.commit(wrong_kind_record,
                                            wrong_kind_loaded.record->revision) ==
                        mt5bridge::StoreCommitStatus::committed,
                "wrong-kind recovery fixture could not persist");
        mt5bridge::OperationJournal wrong_kind_journal(wrong_kind_store);
        const auto wrong_kind_records = wrong_kind_journal.recover_all();
        require(wrong_kind_records.accepted() &&
                    !mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
                        partial_recovery_seed, operation_account, wrong_kind_records),
                "close record was misclassified as settled OPEN exposure");

        MemoryStore late_restart_store = store;
        mt5bridge::OperationJournal late_restart_journal(late_restart_store);
        const auto late_restart_records = late_restart_journal.recover_all();
        require(late_restart_records.accepted(),
                "late-remainder restart scan did not recover durable records");
        auto late_restart_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(713, 910, 911, 1.0)}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(713, 910, 911, 1.0)}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(710, 910, 911, 1.0),
                     history_deal(711, 910, 911, 1.0),
                     history_deal(713, 910, 911, 1.0),
                     history_deal(714, 910, 911, 2.0)})});
        mt5bridge::ObservationCoordinator late_restart_coordinator(
            *late_restart_provider, operation_account);
        mt5bridge::ObservationCollectionRequest late_restart_collection;
        late_restart_collection.history_deals_window = deal_window;
        const auto late_restart_first =
            late_restart_coordinator.refresh(late_restart_collection);
        const auto late_restart_second =
            late_restart_coordinator.refresh(late_restart_collection);
        require(late_restart_first.sample && late_restart_second.sample,
                "late-remainder restart baseline setup failed");
        mt5bridge::DispatchAdmissionBarrier late_restart_admission(
            late_restart_journal, late_restart_coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend late_restart_backend;
        FakeTransport late_restart_transport;
        FakeAccountProbe late_restart_probe({operation_account});
        mt5bridge::dispatch::ManagedTradeOwner late_restart_owner(
            *partial_recovered, operation_account, late_restart_journal,
            late_restart_admission, late_restart_backend, late_restart_probe, lease,
            late_restart_transport);
        mt5bridge::OperationReconciliationWorker late_restart_worker(
            late_restart_journal, *partial_executed.key, late_restart_coordinator,
            late_restart_collection);
        const auto late_restart_settled =
            late_restart_owner.settle_pending_remainder(late_restart_worker);
        require(late_restart_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::partially_filled &&
                    late_restart_settled.record &&
                    late_restart_settled.record->settled_volume == 3 &&
                    late_restart_owner.state().open_volume == 3 &&
                    late_restart_owner.state().pending_remainder_volume == 2 &&
                    late_restart_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled,
                "late remainder restart overcounted the first cumulative snapshot");

        mt5bridge::OperationReconciliationWorker repeated_restart_worker(
            late_restart_journal, *partial_executed.key, late_restart_coordinator,
            late_restart_collection);
        const auto repeated_restart =
            late_restart_owner.settle_pending_remainder(repeated_restart_worker);
        require(repeated_restart.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    repeated_restart.record &&
                    repeated_restart.record->settled_volume == 3 &&
                    late_restart_owner.state().open_volume == 3 &&
                    late_restart_owner.state().pending_remainder_volume == 2,
                "repeated cumulative snapshot advanced the remainder twice");

        mt5bridge::OperationReconciliationWorker final_restart_worker(
            late_restart_journal, *partial_executed.key, late_restart_coordinator,
            late_restart_collection);
        const auto final_restart =
            late_restart_owner.settle_pending_remainder(final_restart_worker);
        require(final_restart.status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    final_restart.record &&
                    final_restart.record->settled_volume == 5 &&
                    late_restart_owner.state().open_volume == 5 &&
                    late_restart_owner.state().pending_remainder_volume == 0 &&
                    late_restart_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::filled,
                "late remainder restart did not complete from cumulative volume");

        mt5bridge::OperationReconciliationWorker ambiguous_late_worker(
            journal, *partial_executed.key, partial_deal_coordinator, deal_collection);
        const auto ambiguous_late =
            partial_owner.settle_pending_remainder(ambiguous_late_worker);
        require(ambiguous_late.status ==
                    mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                    ambiguous_late.record &&
                    ambiguous_late.record->operation_state ==
                        mt5bridge::OperationState::partially_filled &&
                    ambiguous_late.record->settled_volume == 2 &&
                    partial_owner.state().open_volume == 2 &&
                    partial_owner.state().pending_remainder_volume == 3 &&
                    partial_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled,
                "ambiguous remainder evidence discarded confirmed partial exposure");

        mt5bridge::OperationReconciliationWorker late_worker(
            journal, *partial_executed.key, partial_deal_coordinator, deal_collection);
        const auto late_settled = partial_owner.settle_pending_remainder(late_worker);
        require(late_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::partially_filled &&
                    late_settled.record &&
                    late_settled.record->operation_state ==
                        mt5bridge::OperationState::partially_filled &&
                    late_settled.record->settled_volume == 4 &&
                    partial_owner.state().open_volume == 4 &&
                    partial_owner.state().pending_remainder_volume == 1 &&
                    partial_owner.state().slice.result_volume == 4 &&
                    partial_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled,
                "late remainder settlement did not advance the partial exposure");

        mt5bridge::OperationReconciliationWorker final_late_worker(
            journal, *partial_executed.key, partial_deal_coordinator, deal_collection);
        const auto final_late_settled =
            partial_owner.settle_pending_remainder(final_late_worker);
        require(final_late_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    final_late_settled.record &&
                    final_late_settled.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    final_late_settled.record->settled_volume == 5 &&
                    partial_owner.state().open_volume == 5 &&
                    partial_owner.state().pending_remainder_volume == 0 &&
                    partial_owner.state().slice.result_volume == 5 &&
                    partial_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::filled,
                "late remainder settlement did not complete the open slice");

        auto prebound_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window,
                                    {history_deal(720, 920, 921, 5.0)})});
        mt5bridge::ObservationCoordinator prebound_coordinator(
            *prebound_provider, operation_account);
        const auto prebound_first =
            prebound_coordinator.refresh(deal_collection);
        const auto prebound_second =
            prebound_coordinator.refresh(deal_collection);
        require(prebound_first.sample && prebound_second.sample,
                "pre-bound deal baseline setup failed");
        const auto prebound_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*prebound_first.sample, *prebound_second.sample},
                deal_consistency_request);
        require(prebound_consistency.consistent() && prebound_consistency.proof,
                "pre-bound deal environment proof setup failed");
        mt5bridge::DispatchAdmissionBarrier prebound_admission(
            journal, prebound_coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend prebound_backend;
        FakeTransport prebound_transport;
        FakeAccountProbe prebound_probe({operation_account, operation_account});
        mt5bridge::managed_trade::ManagedTradeState prebound_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &prebound_state, mt5bridge::managed_trade::TradeId{52}, 5, 5, 4),
                "pre-bound deal managed state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner prebound_owner(
            prebound_state, operation_account, journal, prebound_admission,
            prebound_backend, prebound_probe, lease, prebound_transport);
        const auto prebound_predicate =
            mt5bridge::expect_reconciliation_transition(
                mt5bridge::ReconciliationPredicateKind::history_deal_present,
                std::uint64_t{720}, false,
                mt5bridge::ReconciliationTransition::absent_to_present, 0,
                deal_window);
        mt5bridge::dispatch::ManagedTradeIntent prebound_intent{
            {0x52},
            {operation_account, prebound_coordinator.capture_baseline(),
             {prebound_predicate}, mt5bridge::OperationState::filled}};
        require(prebound_owner.prepare_open(5, std::move(prebound_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "pre-bound deal owner did not prepare the open slice");
        mt5bridge::DispatchAdmissionRequest prebound_ready;
        prebound_ready.current_account = operation_account;
        prebound_ready.environment_proof = prebound_consistency.proof;
        const auto prebound_executed = prebound_owner.execute_pending(prebound_ready);
        require(prebound_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    prebound_executed.key,
                "pre-bound deal owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker prebound_worker(
            journal, *prebound_executed.key, prebound_coordinator, deal_collection);
        const auto prebound_settled =
            prebound_owner.settle_reconciliation(prebound_worker);
        require(prebound_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    prebound_settled.record &&
                    prebound_settled.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    prebound_settled.record->operation_state ==
                        mt5bridge::OperationState::reconciling &&
                    prebound_owner.state().open_volume == 0,
                "caller-chosen deal ticket incorrectly settled an open slice");

        auto overfill_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(
                    operation_account, deal_window,
                    {history_deal(730, 930, 931, 3.0),
                     history_deal(731, 930, 931, 3.0)})});
        mt5bridge::ObservationCoordinator overfill_coordinator(
            *overfill_provider, operation_account);
        const auto overfill_first = overfill_coordinator.refresh(deal_collection);
        const auto overfill_second = overfill_coordinator.refresh(deal_collection);
        require(overfill_first.sample && overfill_second.sample,
                "overfill baseline setup failed");
        const auto overfill_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*overfill_first.sample, *overfill_second.sample},
                deal_consistency_request);
        require(overfill_consistency.consistent() && overfill_consistency.proof,
                "overfill environment proof setup failed");
        mt5bridge::DispatchAdmissionBarrier overfill_admission(
            journal, overfill_coordinator.graph(), scope);
        mt5bridge::runtime::OneShotDispatchBackend overfill_backend;
        FakeTransport overfill_transport;
        overfill_transport.next.reconciliation_bindings = {{701, 730}};
        FakeAccountProbe overfill_probe({operation_account, operation_account});
        mt5bridge::managed_trade::ManagedTradeState overfill_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &overfill_state, mt5bridge::managed_trade::TradeId{53}, 5, 5, 4),
                "overfill managed state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner overfill_owner(
            overfill_state, operation_account, journal, overfill_admission,
            overfill_backend, overfill_probe, lease, overfill_transport);
        const auto overfill_predicate =
            mt5bridge::expect_reconciliation_transition(
                mt5bridge::ReconciliationPredicateKind::history_deal_present,
                std::nullopt, false,
                mt5bridge::ReconciliationTransition::absent_to_present, 701,
                deal_window);
        mt5bridge::dispatch::ManagedTradeIntent overfill_intent{
            {0x53},
            {operation_account, overfill_coordinator.capture_baseline(),
             {overfill_predicate}, mt5bridge::OperationState::filled}};
        require(overfill_owner.prepare_open(5, std::move(overfill_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "overfill owner did not prepare the open slice");
        mt5bridge::DispatchAdmissionRequest overfill_ready;
        overfill_ready.current_account = operation_account;
        overfill_ready.environment_proof = overfill_consistency.proof;
        const auto overfill_executed = overfill_owner.execute_pending(overfill_ready);
        require(overfill_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    overfill_executed.key,
                "overfill owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker overfill_worker(
            journal, *overfill_executed.key, overfill_coordinator, deal_collection);
        const auto overfill_settled =
            overfill_owner.settle_reconciliation(overfill_worker);
        require(overfill_settled.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    overfill_settled.record &&
                    overfill_settled.record->journal_state ==
                        mt5bridge::JournalState::reconciling &&
                    overfill_settled.record->operation_state ==
                        mt5bridge::OperationState::reconciling &&
                    overfill_owner.state().open_volume == 0,
                "overfilled deal evidence was silently capped into a fill");

        auto full_open_state = [](std::uint64_t trade_id) {
            mt5bridge::managed_trade::ManagedTradeState state;
            require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                        &state, mt5bridge::managed_trade::TradeId{trade_id}, 5, 5, 6),
                    "exit state initialization failed");
            require(state.start_open_slice(5) ==
                        mt5bridge::managed_trade::MutationStatus::applied &&
                        state.enter_submitting() ==
                            mt5bridge::managed_trade::MutationStatus::applied &&
                        state.record_fill(5) ==
                            mt5bridge::managed_trade::MutationStatus::applied &&
                        state.reconcile() ==
                            mt5bridge::managed_trade::MutationStatus::applied,
                    "exit state full-open setup failed");
            return state;
        };

        MemoryStore close_store;
        auto close_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window, {}),
                history_deals_batch(operation_account, deal_window,
                                    {history_deal(801, 901, 902, 2.0, 1)}),
                history_deals_batch(operation_account, deal_window,
                                    {history_deal(801, 901, 902, 2.0, 1),
                                     history_deal(803, 901, 902, 1.0, 1)})});
        mt5bridge::ObservationCoordinator close_coordinator(
            *close_provider, operation_account);
        const auto close_first = close_coordinator.refresh(deal_collection);
        const auto close_second = close_coordinator.refresh(deal_collection);
        require(close_first.sample && close_second.sample,
                "close baseline setup failed");
        mt5bridge::EnvironmentConsistencyRequest close_consistency_request;
        close_consistency_request.history_deals_window = deal_window;
        const auto close_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*close_first.sample, *close_second.sample},
                close_consistency_request);
        require(close_consistency.consistent() && close_consistency.proof,
                "close consistency proof setup failed");
        mt5bridge::OperationJournal close_journal(close_store);
        mt5bridge::DispatchAdmissionBarrier close_admission(
            close_journal, close_coordinator.graph(), close_consistency_request);
        mt5bridge::runtime::OneShotDispatchBackend close_backend;
        FakeTransport close_transport;
        close_transport.next.reconciliation_bindings = {{9001, 801}};
        FakeAccountProbe close_probe({operation_account, operation_account});
        mt5bridge::dispatch::ManagedTradeOwner close_owner(
            full_open_state(60), operation_account, close_journal,
            close_admission, close_backend, close_probe, lease, close_transport);
        const auto close_predicate = mt5bridge::expect_reconciliation_transition(
            mt5bridge::ReconciliationPredicateKind::history_deal_present,
            std::nullopt, false, mt5bridge::ReconciliationTransition::absent_to_present,
            9001, deal_window);
        mt5bridge::dispatch::ManagedTradeIntent close_intent{
            {0xC1},
            {operation_account, close_coordinator.capture_baseline(),
             {close_predicate}, mt5bridge::OperationState::partially_filled}};
        require(close_owner.prepare_close(3, std::move(close_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "close owner did not prepare a durable close slice");
        mt5bridge::DispatchAdmissionRequest close_ready;
        close_ready.current_account = operation_account;
        close_ready.environment_proof = close_consistency.proof;
        const auto close_executed = close_owner.execute_pending(close_ready);
        require(close_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    close_executed.key && close_owner.state().slice.kind ==
                        mt5bridge::managed_trade::OperationKind::close,
                "close owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker close_worker(
            close_journal, *close_executed.key, close_coordinator, deal_collection);
        const auto close_partial =
            close_owner.settle_close_reconciliation(close_worker);
        require(close_partial.status ==
                    mt5bridge::dispatch::OwnerStepStatus::partially_filled &&
                    close_partial.record && close_partial.record->operation_kind ==
                        mt5bridge::OperationKind::close &&
                    close_partial.record->settled_volume == 2 &&
                    close_owner.state().open_volume == 3 &&
                    close_owner.state().close_obligation.requested &&
                    close_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::partially_filled,
                "provenance-bearing partial close did not reduce exposure");

        mt5bridge::dispatch::ManagedTradeIntent premature_close_intent{
            {0xC2},
            {operation_account, close_coordinator.capture_baseline(),
             {close_predicate}, mt5bridge::OperationState::filled}};
        require(close_owner.prepare_close(1, std::move(premature_close_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::invalid_state,
                "partial close incorrectly permitted a replacement operation");

        mt5bridge::OperationReconciliationWorker late_close_worker(
            close_journal, *close_executed.key, close_coordinator, deal_collection);
        const auto late_close =
            close_owner.settle_close_reconciliation(late_close_worker);
        require(late_close.status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    late_close.record && late_close.record->operation_state ==
                        mt5bridge::OperationState::filled &&
                    late_close.record->settled_volume == 3 &&
                    close_owner.state().open_volume == 2 &&
                    close_owner.state().close_obligation.requested &&
                    close_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::filled,
                "late close evidence did not complete the original close operation");
        mt5bridge::dispatch::ManagedTradeIntent replacement_close_intent{
            {0xC2},
            {operation_account, close_coordinator.capture_baseline(),
             {close_predicate}, mt5bridge::OperationState::filled}};
        require(close_owner.prepare_close(2, std::move(replacement_close_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "terminal close did not permit a replacement close");

        MemoryStore cancel_store;
        mt5bridge::managed_trade::ManagedTradeState cancel_state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &cancel_state, mt5bridge::managed_trade::TradeId{61}, 5, 5, 6),
                "cancel state initialization failed");
        require(cancel_state.start_open_slice(4) ==
                    mt5bridge::managed_trade::MutationStatus::applied &&
                    cancel_state.enter_submitting() ==
                        mt5bridge::managed_trade::MutationStatus::applied &&
                    cancel_state.record_fill(2) ==
                        mt5bridge::managed_trade::MutationStatus::applied &&
                    cancel_state.reconcile() ==
                        mt5bridge::managed_trade::MutationStatus::applied &&
                    cancel_state.pending_remainder_volume == 2,
                "cancel state partial-open setup failed");
        auto cancel_batch = [&](std::vector<Mt5DealSnapshot> deals, bool active) {
            auto batch = history_deals_batch(operation_account, deal_window,
                                              std::move(deals));
            if (active) {
                Mt5OrderSnapshot order{};
                order.ticket = 901;
                order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                                     MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
                batch.active_orders.push_back(order);
            }
            return batch;
        };
        auto cancel_provider = std::make_unique<FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                cancel_batch({history_deal(1001, 901, 902, 2.0)}, true),
                cancel_batch({history_deal(1001, 901, 902, 2.0)}, true),
                cancel_batch({history_deal(1001, 901, 902, 2.0),
                              history_deal(1002, 901, 902, 2.0)}, false)});
        mt5bridge::ObservationCoordinator cancel_coordinator(
            *cancel_provider, operation_account);
        mt5bridge::ObservationCollectionRequest cancel_collection;
        cancel_collection.history_deals_window = deal_window;
        const auto cancel_first = cancel_coordinator.refresh(cancel_collection);
        const auto cancel_second = cancel_coordinator.refresh(cancel_collection);
        require(cancel_first.sample && cancel_second.sample,
                "cancel baseline setup failed");
        mt5bridge::EnvironmentConsistencyRequest cancel_consistency_request;
        cancel_consistency_request.history_deals_window = deal_window;
        const auto cancel_consistency =
            mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*cancel_first.sample, *cancel_second.sample},
                cancel_consistency_request);
        require(cancel_consistency.consistent() && cancel_consistency.proof,
                "cancel consistency proof setup failed");
        mt5bridge::ObservationGraph cancel_open_graph(operation_account);
        const auto cancel_open_baseline =
            mt5bridge::capture_reconciliation_baseline(cancel_open_graph);
        mt5bridge::OperationRecord cancel_open_record;
        cancel_open_record.key = {operation_account, 61, 1};
        cancel_open_record.operation_kind = mt5bridge::OperationKind::open;
        cancel_open_record.request_payload = {0xC0};
        cancel_open_record.result_payload = {0xC1};
        cancel_open_record.operation_state =
            mt5bridge::OperationState::partially_filled;
        cancel_open_record.journal_state = mt5bridge::JournalState::reconciling;
        cancel_open_record.revision = 1;
        cancel_open_record.fencing_token = 1;
        cancel_open_record.settled_volume = 2;
        cancel_open_record.reconciliation_descriptor =
            mt5bridge::ReconciliationDescriptor{
                operation_account, cancel_open_baseline,
                {mt5bridge::require_active_order(901),
                 mt5bridge::expect_reconciliation_transition(
                     mt5bridge::ReconciliationPredicateKind::history_deal_present,
                     std::uint64_t{1001}, false,
                     mt5bridge::ReconciliationTransition::absent_to_present, 0,
                     deal_window)},
                mt5bridge::OperationState::partially_filled, 61, 1, 4};
        require(cancel_open_record.valid() &&
                    cancel_store.commit(cancel_open_record, std::nullopt) ==
                        mt5bridge::StoreCommitStatus::committed,
                "cancel OPEN frontier fixture was not committed");
        mt5bridge::OperationJournal cancel_journal(cancel_store);
        require(cancel_journal.recover(cancel_open_record.key).accepted(),
                "cancel OPEN frontier was not recovered into the journal");
        mt5bridge::DispatchAdmissionBarrier cancel_admission(
            cancel_journal, cancel_coordinator.graph(), cancel_consistency_request);
        mt5bridge::runtime::OneShotDispatchBackend cancel_backend;
        FakeTransport cancel_transport;
        FakeAccountProbe cancel_probe({operation_account, operation_account});
        mt5bridge::dispatch::ManagedTradeOwner cancel_owner(
            cancel_state, operation_account, cancel_journal, cancel_admission,
            cancel_backend, cancel_probe, lease, cancel_transport);
        mt5bridge::dispatch::ManagedTradeIntent cancel_intent{
            {0xCA},
            {operation_account, cancel_coordinator.capture_baseline(),
             {mt5bridge::require_active_order_absent(901)},
             mt5bridge::OperationState::cancelled}};
        require(cancel_owner.prepare_cancel(std::move(cancel_intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "cancel owner did not prepare a durable cancellation");
        mt5bridge::DispatchAdmissionRequest cancel_ready;
        cancel_ready.current_account = operation_account;
        cancel_ready.environment_proof = cancel_consistency.proof;
        const auto cancel_executed = cancel_owner.execute_pending(cancel_ready);
        require(cancel_executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    cancel_executed.key,
                "cancel owner did not reach reconciliation");
        mt5bridge::OperationReconciliationWorker cancel_worker(
            cancel_journal, *cancel_executed.key, cancel_coordinator,
            cancel_collection);
        const auto cancelled =
            cancel_owner.settle_cancel_reconciliation(cancel_worker);
        require(cancelled.status == mt5bridge::dispatch::OwnerStepStatus::completed &&
                    cancelled.record && cancelled.record->operation_kind ==
                        mt5bridge::OperationKind::cancel &&
                    cancelled.record->operation_state ==
                        mt5bridge::OperationState::cancelled &&
                    cancel_owner.state().pending_remainder_volume == 0 &&
                    cancel_owner.state().open_volume == 4 &&
                    cancel_owner.state().slice.state ==
                        mt5bridge::managed_trade::OperationState::cancelled,
                "cancel settlement did not preserve filled exposure");
        const auto persisted_cancel_open = cancel_journal.find(
            {operation_account, 61, 1});
        require(persisted_cancel_open &&
                    persisted_cancel_open->operation_state ==
                        mt5bridge::OperationState::filled &&
                    persisted_cancel_open->settled_volume == 4 &&
                    persisted_cancel_open->valid(),
                "late cancel fill was not persisted on the OPEN frontier");
        const auto cancel_recovery = cancel_journal.recover_all();
        mt5bridge::managed_trade::ManagedTradeState cancel_recovery_seed;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &cancel_recovery_seed, mt5bridge::managed_trade::TradeId{61}, 5, 5,
                    6),
                "cancel recovery seed initialization failed");
        const auto recovered_cancel =
            mt5bridge::dispatch::ManagedTradeOwner::recover_settled_trade(
                cancel_recovery_seed, operation_account, cancel_recovery);
        require(recovered_cancel && recovered_cancel->valid() &&
                    recovered_cancel->open_volume == 4 &&
                    recovered_cancel->pending_remainder_volume == 0 &&
                    recovered_cancel->slice.operation_id == 2 &&
                    recovered_cancel->slice.kind ==
                        mt5bridge::managed_trade::OperationKind::cancel &&
                    recovered_cancel->slice.state ==
                        mt5bridge::managed_trade::OperationState::cancelled,
                "restart did not preserve a late fill before cancellation");

        MemoryStore exit_recovery_store;
        const auto recovery_graph = mt5bridge::ObservationGraph(operation_account);
        const auto recovery_baseline =
            mt5bridge::capture_reconciliation_baseline(recovery_graph);
        auto durable_exit_record = [&](std::uint64_t trade_id,
                                       std::uint64_t operation_id,
                                       mt5bridge::OperationKind kind,
                                       mt5bridge::OperationState state,
                                       std::uint64_t requested,
                                       std::uint64_t settled,
                                       mt5bridge::OperationState settled_state) {
            mt5bridge::OperationRecord record;
            record.key = {operation_account, trade_id, operation_id};
            record.operation_kind = kind;
            record.request_payload = {0xE1};
            record.result_payload = {0xE2};
            record.operation_state = state;
            record.journal_state = mt5bridge::JournalState::reconciling;
            record.revision = 1;
            record.fencing_token = 1;
            record.settled_volume = settled;
            record.reconciliation_descriptor = mt5bridge::ReconciliationDescriptor{
                operation_account, recovery_baseline,
                {mt5bridge::require_active_order(1000 + operation_id)}, settled_state,
                trade_id, operation_id, requested};
            require(record.valid(), "durable exit recovery fixture is invalid");
            require(exit_recovery_store.commit(record, std::nullopt) ==
                        mt5bridge::StoreCommitStatus::committed,
                    "durable exit recovery fixture was not committed");
        };
        durable_exit_record(61, 1, mt5bridge::OperationKind::open,
                            mt5bridge::OperationState::partially_filled, 4, 2,
                            mt5bridge::OperationState::partially_filled);
        durable_exit_record(61, 2, mt5bridge::OperationKind::cancel,
                            mt5bridge::OperationState::cancelled, 2, 0,
                            mt5bridge::OperationState::cancelled);
        durable_exit_record(61, 3, mt5bridge::OperationKind::close,
                            mt5bridge::OperationState::filled, 2, 2,
                            mt5bridge::OperationState::filled);
        mt5bridge::OperationJournal exit_recovery_journal(exit_recovery_store);
        const auto exit_recovery = exit_recovery_journal.recover_all();
        mt5bridge::managed_trade::ManagedTradeState exit_seed;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &exit_seed, mt5bridge::managed_trade::TradeId{61}, 5, 5, 6),
                "durable exit recovery seed initialization failed");
        const auto recovered_exit =
            mt5bridge::dispatch::ManagedTradeOwner::recover_settled_trade(
                exit_seed, operation_account, exit_recovery);
        require(recovered_exit && recovered_exit->valid() &&
                    recovered_exit->open_volume == 0 &&
                    recovered_exit->pending_remainder_volume == 0 &&
                    recovered_exit->close_obligation.satisfied &&
                    recovered_exit->slice.operation_id == 3 &&
                    recovered_exit->slice.kind ==
                        mt5bridge::managed_trade::OperationKind::close &&
                    recovered_exit->slice.state ==
                        mt5bridge::managed_trade::OperationState::filled,
                "restart did not reconstruct OPEN/CANCEL/CLOSE exposure");

        mt5bridge::OperationJournal restarted_journal(store);
        const auto recovered_records = restarted_journal.recover_all();
        require(recovered_records.accepted(),
                "managed-trade restart scan did not recover durable records");
        mt5bridge::managed_trade::ManagedTradeState recovered_full_seed;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &recovered_full_seed, mt5bridge::managed_trade::TradeId{50}, 5, 5, 4),
                "full-fill recovery seed initialization failed");
        const auto recovered_full =
            mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
                recovered_full_seed, operation_account, recovered_records);
        require(recovered_full && recovered_full->valid() &&
                    recovered_full->slice.state ==
                        mt5bridge::managed_trade::OperationState::filled &&
                    recovered_full->open_volume == 5 &&
                    recovered_full->pending_remainder_volume == 0,
                "durable full fill did not reconstruct managed exposure");

        mt5bridge::managed_trade::ManagedTradeState recovered_complete_seed;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &recovered_complete_seed, mt5bridge::managed_trade::TradeId{51}, 5, 5,
                    4),
                "completed-fill recovery seed initialization failed");
        const auto recovered_complete =
            mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
                recovered_complete_seed, operation_account, recovered_records);
        require(recovered_complete && recovered_complete->valid() &&
                    recovered_complete->slice.state ==
                        mt5bridge::managed_trade::OperationState::filled &&
                    recovered_complete->open_volume == 5 &&
                    recovered_complete->pending_remainder_volume == 0,
                "durable completed fill did not reconstruct managed exposure");

        require(recovered_records.accepted() && !recovered_records.records().empty(),
                "accepted recovery did not expose a complete read-only record view");
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

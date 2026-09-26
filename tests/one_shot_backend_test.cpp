/// \file one_shot_backend_test.cpp
/// \brief Exercises guarded one-shot backend execution and broker rejection.

#include "one_shot_backend.hpp"

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

mt5bridge::AccountKey account() { return {"Demo-Trade", 42}; }

mt5bridge::OperationKey key(std::uint64_t trade_id) {
    return {account(), trade_id, trade_id + 100};
}

class MemoryStore final : public mt5bridge::DurableJournalStore {
public:
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        const auto it = durable.find(record.key);
        if (expected_revision) {
            if (it == durable.end() || it->second.revision != *expected_revision)
                return mt5bridge::StoreCommitStatus::conflict;
        } else if (it != durable.end()) {
            return mt5bridge::StoreCommitStatus::conflict;
        }
        durable[record.key] = record;
        return mt5bridge::StoreCommitStatus::committed;
    }

    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &operation_key) const override {
        const auto it = durable.find(operation_key);
        return it == durable.end()
                   ? mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::not_found,
                                                std::nullopt}
                   : mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::found, it->second};
    }

    mt5bridge::StoreScanResult scan() const override {
        mt5bridge::StoreScanResult result;
        result.status = mt5bridge::StoreScanStatus::complete;
        for (const auto &entry : durable)
            result.records.push_back(entry.second);
        return result;
    }

private:
    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> durable;
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

mt5bridge::ObservationBatch observation_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    Mt5OrderSnapshot order{};
    order.ticket = 20;
    order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    batch.active_orders.push_back(order);
    return batch;
}

class FakeLease final : public mt5bridge::SingleWriterLease {
public:
    explicit FakeLease(mt5bridge::AccountKey account) : owned_account(std::move(account)) {}

    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &requested) const override {
        ++calls;
        if (!held || requested != owned_account ||
            (drop_before_call && calls >= 3))
            return std::nullopt;
        return token;
    }

    mt5bridge::AccountKey owned_account;
    std::uint64_t token = 77;
    mutable std::size_t calls = 0;
    bool held = true;
    bool drop_before_call = false;
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
        const mt5bridge::OperationRecord &record) override {
        ++calls;
        last_request = record.request_payload;
        return next;
    }

    mt5bridge::runtime::BackendCallResult next{
        mt5bridge::runtime::BackendCallStatus::broker_result,
        mt5bridge::runtime::BrokerResultDisposition::reconciling,
        0,
        {0xA0, 0x01}};
    std::size_t calls = 0;
    std::vector<std::uint8_t> last_request;
};

void prepare_for_admission(mt5bridge::OperationJournal &journal,
                           const mt5bridge::OperationKey &operation_key,
                           mt5bridge::ReconciliationDescriptor descriptor) {
    require(journal.transition_operation(operation_key,
                                         mt5bridge::OperationState::prechecking)
                .accepted(),
            "prechecking transition failed");
    require(journal.transition_journal(operation_key, mt5bridge::JournalState::prechecked)
                .accepted(),
            "prechecked transition failed");
    require(journal.transition_journal(
                operation_key, mt5bridge::JournalState::dispatch_intent_persisted)
                .accepted(),
            "dispatch intent transition failed");
    require(journal.persist_reconciliation_descriptor(
                operation_key, std::move(descriptor))
                .accepted(),
            "reconciliation descriptor transition failed");
}

std::optional<mt5bridge::DispatchPermit> admit(
    mt5bridge::OperationJournal &journal, const mt5bridge::ObservationGraph &graph,
    const mt5bridge::OperationKey &operation_key,
    const mt5bridge::EnvironmentConsistencyProof &proof, FakeLease &lease) {
    mt5bridge::EnvironmentConsistencyRequest scope;
    mt5bridge::DispatchAdmissionBarrier barrier(journal, graph, scope);
    mt5bridge::DispatchAdmissionRequest request;
    request.current_account = operation_key.account;
    request.environment_proof = proof;
    auto result = barrier.admit(operation_key, request, lease);
    if (!result.admitted())
        return std::nullopt;
    return std::move(result.permit);
}

} // namespace

/// \brief Runs one-shot backend safety and broker rejection checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    try {
        const auto operation_account = account();
        FakeObservationProvider provider(
            {observation_batch(operation_account), observation_batch(operation_account)});
        mt5bridge::ObservationCoordinator coordinator(provider, operation_account);
        mt5bridge::ObservationCollectionRequest collection;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample, "observation setup failed");
        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, consistency_request);
        require(consistency.consistent() && consistency.proof,
                "consistent proof setup failed");
        const auto descriptor = mt5bridge::ReconciliationDescriptor{
            operation_account, coordinator.capture_baseline(),
            {mt5bridge::require_active_order(999)}, mt5bridge::OperationState::filled};

        MemoryStore store;
        mt5bridge::OperationJournal journal(store);
        const auto rejected_key = key(7);
        require(journal.create(rejected_key, {0x01, 0x02}).accepted(),
                "rejection operation create failed");
        prepare_for_admission(journal, rejected_key, descriptor);
        FakeLease lease{operation_account};
        auto permit = admit(journal, coordinator.graph(), rejected_key,
                            *consistency.proof, lease);
        require(permit.has_value(), "dispatch permit setup failed");

        FakeTransport transport;
        transport.next.retcode = mt5bridge::runtime::kTradeRetcodeMarketClosed;
        transport.next.disposition =
            mt5bridge::runtime::BrokerResultDisposition::rejected;
        mt5bridge::runtime::OneShotDispatchBackend backend;
        FakeAccountProbe rejected_account_probe({operation_account, operation_account});
        auto executed = backend.execute(journal, rejected_key, std::move(*permit),
                                         rejected_account_probe, lease, transport);
        require(executed.completed() && executed.retcode == 10018 &&
                    executed.record &&
                    executed.record->operation_state == mt5bridge::OperationState::rejected &&
                    executed.record->journal_state == mt5bridge::JournalState::result_persisted &&
                    executed.record->result_payload == std::vector<std::uint8_t>({0xA0, 0x01}) &&
                    transport.calls == 1,
                "market-closed broker rejection was not durably classified");
        auto retry = backend.execute(journal, rejected_key, std::move(*permit),
                                     rejected_account_probe, lease, transport);
        require(retry.status == mt5bridge::runtime::OneShotExecutionStatus::invalid_permit &&
                    transport.calls == 1,
                "consumed permit enabled a second backend call");

        const auto binding_key = key(12);
        require(journal.create(binding_key, {0x06}).accepted(),
                "binding operation create failed");
        const auto unknown_descriptor = mt5bridge::ReconciliationDescriptor{
            operation_account, coordinator.capture_baseline(),
            {mt5bridge::expect_reconciliation_transition(
                mt5bridge::ReconciliationPredicateKind::active_order_present,
                std::nullopt, false,
                mt5bridge::ReconciliationTransition::absent_to_present, 12001)},
            mt5bridge::OperationState::filled};
        prepare_for_admission(journal, binding_key, unknown_descriptor);
        FakeLease binding_lease{operation_account};
        auto binding_permit = admit(journal, coordinator.graph(), binding_key,
                                    *consistency.proof, binding_lease);
        require(binding_permit.has_value(), "binding permit setup failed");
        FakeTransport binding_transport;
        binding_transport.next.reconciliation_bindings.push_back({12001, 999});
        FakeAccountProbe binding_account_probe({operation_account, operation_account});
        const auto binding_result = backend.execute(
            journal, binding_key, std::move(*binding_permit), binding_account_probe,
            binding_lease, binding_transport);
        require(binding_result.completed() && binding_result.record &&
                    binding_result.record->result_payload ==
                        std::vector<std::uint8_t>({0xA0, 0x01}) &&
                    binding_result.record->reconciliation_bindings.size() == 1 &&
                    binding_result.record->reconciliation_bindings.front() ==
                        mt5bridge::ReconciliationBinding{12001, 999},
                "validated broker result did not create a durable ticket binding");
        mt5bridge::OperationJournal binding_recovered(store);
        const auto recovered_binding = binding_recovered.recover(binding_key);
        require(recovered_binding.accepted() && recovered_binding.record &&
                    recovered_binding.record->result_payload ==
                        std::vector<std::uint8_t>({0xA0, 0x01}) &&
                    recovered_binding.record->reconciliation_bindings.size() == 1,
                "durable ticket binding did not survive journal recovery");

        const auto missing_binding_key = key(14);
        require(journal.create(missing_binding_key, {0x06, 0x01}).accepted(),
                "missing-binding operation create failed");
        prepare_for_admission(journal, missing_binding_key, unknown_descriptor);
        FakeLease missing_binding_lease{operation_account};
        auto missing_binding_permit =
            admit(journal, coordinator.graph(), missing_binding_key, *consistency.proof,
                  missing_binding_lease);
        require(missing_binding_permit.has_value(),
                "missing-binding permit setup failed");
        FakeTransport missing_binding_transport;
        FakeAccountProbe missing_binding_probe({operation_account, operation_account});
        const auto missing_binding_result = backend.execute(
            journal, missing_binding_key, std::move(*missing_binding_permit),
            missing_binding_probe, missing_binding_lease, missing_binding_transport);
        require(missing_binding_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::
                        reconciliation_binding_failed &&
                    missing_binding_result.record &&
                    missing_binding_result.record->result_payload.empty() &&
                    missing_binding_result.record->operation_state ==
                        mt5bridge::OperationState::submitting &&
                    missing_binding_transport.calls == 1,
                "missing result-derived binding was persisted without atomic identity evidence");

        const auto binding_conflict_key = key(13);
        require(journal.create(binding_conflict_key, {0x07}).accepted(),
                "binding-conflict operation create failed");
        const auto conflicting_descriptor = mt5bridge::ReconciliationDescriptor{
            operation_account, coordinator.capture_baseline(),
            {mt5bridge::expect_reconciliation_transition(
                mt5bridge::ReconciliationPredicateKind::active_order_present,
                std::nullopt, false,
                mt5bridge::ReconciliationTransition::absent_to_present, 13001)},
            mt5bridge::OperationState::filled};
        prepare_for_admission(journal, binding_conflict_key, conflicting_descriptor);
        FakeLease binding_conflict_lease{operation_account};
        auto binding_conflict_permit = admit(
            journal, coordinator.graph(), binding_conflict_key, *consistency.proof,
            binding_conflict_lease);
        require(binding_conflict_permit.has_value(),
                "binding-conflict permit setup failed");
        FakeTransport binding_conflict_transport;
        binding_conflict_transport.next.reconciliation_bindings = {
            {13001, 998}, {13001, 999}};
        FakeAccountProbe binding_conflict_probe({operation_account, operation_account});
        const auto binding_conflict_result = backend.execute(
            journal, binding_conflict_key, std::move(*binding_conflict_permit),
            binding_conflict_probe, binding_conflict_lease, binding_conflict_transport);
        require(binding_conflict_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::reconciliation_binding_failed &&
                    binding_conflict_result.record &&
                    binding_conflict_result.record->reconciliation_bindings.empty() &&
                    binding_conflict_result.record->result_payload.empty() &&
                    binding_conflict_result.record->operation_state ==
                        mt5bridge::OperationState::submitting,
                "conflicting ticket binding escaped the atomic result commit");

        const auto stale_key = key(8);
        require(journal.create(stale_key, {0x03}).accepted(),
                "stale-permit operation create failed");
        prepare_for_admission(journal, stale_key, descriptor);
        FakeLease stale_lease{operation_account};
        auto stale_permit = admit(journal, coordinator.graph(), stale_key,
                                  *consistency.proof, stale_lease);
        require(stale_permit.has_value(), "stale-permit setup failed");
        require(journal.transition_operation(stale_key,
                                             mt5bridge::OperationState::submitting)
                    .accepted(),
                "stale-permit mutation setup failed");
        FakeTransport stale_transport;
        FakeAccountProbe stale_account_probe({operation_account});
        const auto stale_result = backend.execute(
            journal, stale_key, std::move(*stale_permit), stale_account_probe,
            stale_lease, stale_transport);
        require(stale_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::stale_permit &&
                    stale_transport.calls == 0,
                "stale permit reached the transport");

        const auto lease_key = key(9);
        require(journal.create(lease_key, {0x03}).accepted(),
                "lease-loss operation create failed");
        prepare_for_admission(journal, lease_key, descriptor);
        FakeLease dropping_lease{operation_account};
        dropping_lease.drop_before_call = true;
        auto lease_permit = admit(journal, coordinator.graph(), lease_key,
                                  *consistency.proof, dropping_lease);
        require(lease_permit.has_value(), "lease-loss permit setup failed");
        FakeTransport lease_transport;
        FakeAccountProbe lease_account_probe({operation_account, operation_account});
        const auto lease_result = backend.execute(
            journal, lease_key, std::move(*lease_permit), lease_account_probe,
            dropping_lease, lease_transport);
        require(lease_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::lease_lost &&
                    lease_transport.calls == 0 &&
                    journal.find(lease_key)->operation_state ==
                        mt5bridge::OperationState::submitting,
                "lease loss before side effect was not fail-closed");

        const auto transport_key = key(10);
        require(journal.create(transport_key, {0x04}).accepted(),
                "transport-failure operation create failed");
        prepare_for_admission(journal, transport_key, descriptor);
        FakeLease transport_lease{operation_account};
        auto transport_permit = admit(journal, coordinator.graph(), transport_key,
                                      *consistency.proof, transport_lease);
        require(transport_permit.has_value(), "transport-failure permit setup failed");
        FakeTransport failing_transport;
        failing_transport.next.status =
            mt5bridge::runtime::BackendCallStatus::transport_failure;
        failing_transport.next.retcode = mt5bridge::runtime::kTradeRetcodeMarketClosed;
        failing_transport.next.raw_result.clear();
        FakeAccountProbe transport_account_probe({operation_account, operation_account});
        const auto transport_result = backend.execute(
            journal, transport_key, std::move(*transport_permit), transport_account_probe,
            transport_lease, failing_transport);
        require(transport_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::transport_failure &&
                    transport_result.retcode == 0 &&
                    failing_transport.calls == 1 &&
                    journal.find(transport_key)->journal_state ==
                        mt5bridge::JournalState::dispatching &&
                    journal.find(transport_key)->operation_state ==
                        mt5bridge::OperationState::submitting,
                "transport failure reopened or retried the operation");

        const auto account_switch_key = key(11);
        require(journal.create(account_switch_key, {0x05}).accepted(),
                "account-switch operation create failed");
        prepare_for_admission(journal, account_switch_key, descriptor);
        FakeLease account_switch_lease{operation_account};
        auto account_switch_permit = admit(journal, coordinator.graph(), account_switch_key,
                                            *consistency.proof, account_switch_lease);
        require(account_switch_permit.has_value(), "account-switch permit setup failed");
        FakeAccountProbe switching_account_probe({operation_account,
                                                  mt5bridge::AccountKey{"Other-Trade", 43}});
        FakeTransport account_switch_transport;
        const auto account_switch_result = backend.execute(
            journal, account_switch_key, std::move(*account_switch_permit),
            switching_account_probe, account_switch_lease, account_switch_transport);
        require(account_switch_result.status ==
                    mt5bridge::runtime::OneShotExecutionStatus::account_mismatch &&
                    account_switch_transport.calls == 0 &&
                    journal.find(account_switch_key)->operation_state ==
                        mt5bridge::OperationState::submitting,
                "account switch after durable submitting reached the transport");

        std::cout << "one-shot backend checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "one-shot backend checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

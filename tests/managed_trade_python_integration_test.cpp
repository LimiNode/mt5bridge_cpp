/// \file managed_trade_python_integration_test.cpp
/// \brief Exercises the private owner loop through the real Python transport.

#include "managed_trade_owner.hpp"
#include "python_dispatch_transport.hpp"

#include "python_ref.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>
#include <mt5bridge/dispatch/operation_recovery.hpp>

#include "support/fake_observation_provider.hpp"

#include <Python.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Fake-Server", 42}; }

mt5bridge::ObservationBatch empty_batch(const mt5bridge::AccountKey &key,
                                        mt5bridge::ObservationWindow window) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions |
                             mt5bridge::ObservationDomain::history_deals;
    batch.position_history_order =
        mt5bridge::PositionHistoryOrder::positions_before_history_deals;
    batch.history_deals_window = window;
    return batch;
}

mt5bridge::ObservationBatch settled_batch(const mt5bridge::AccountKey &key,
                                          mt5bridge::ObservationWindow window) {
    auto batch = empty_batch(key, window);
    Mt5OrderSnapshot order{};
    order.ticket = 7001;
    order.position_id = 9001;
    order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    batch.active_orders.push_back(order);

    Mt5DealSnapshot deal{};
    deal.ticket = 7002;
    deal.order_ticket = 7001;
    deal.position_id = 9001;
    deal.entry = 0;
    deal.volume = 1.0;
    deal.time_msc = 2000;
    deal.known_fields = MT5BRIDGE_DEAL_KNOWN_TICKET |
                        MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET |
                        MT5BRIDGE_DEAL_KNOWN_POSITION_ID |
                        MT5BRIDGE_DEAL_KNOWN_ENTRY |
                        MT5BRIDGE_DEAL_KNOWN_VOLUME |
                        MT5BRIDGE_DEAL_KNOWN_TIME;
    batch.history_deals.push_back(deal);
    return batch;
}

mt5bridge::ReconciliationDescriptor descriptor(
    const mt5bridge::AccountKey &key, const mt5bridge::ReconciliationBaseline &baseline,
    mt5bridge::ObservationWindow window) {
    return {key,
            baseline,
            {mt5bridge::expect_reconciliation_transition(
                 mt5bridge::ReconciliationPredicateKind::active_order_present,
                 std::nullopt, false,
                 mt5bridge::ReconciliationTransition::absent_to_present, 7001),
             mt5bridge::expect_reconciliation_transition(
                 mt5bridge::ReconciliationPredicateKind::history_deal_present,
                 std::nullopt, false,
                 mt5bridge::ReconciliationTransition::absent_to_present, 7002,
                 window)},
            mt5bridge::OperationState::filled, 7, 1};
}

void check_order_calls(PyObject *module) {
    mt5bridge::runtime::PyRef calls(PyObject_GetAttrString(module, "order_calls"));
    require(calls && PyLong_Check(calls.get()) && PyLong_AsLong(calls.get()) == 1,
            "Python adapter did not perform exactly one order_send call");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      "mt5bridge-managed-trade-python-integration";
    std::filesystem::remove_all(root);
    try {
        Py_Initialize();
        require(Py_IsInitialized() != 0, "CPython initialization failed");
        PyObject *module = PyImport_ImportModule("MetaTrader5");
        require(module != nullptr, "fake MetaTrader5 module import failed");
        {

        const auto operation_account = account();
        const mt5bridge::ObservationWindow window{1000, 3000};
        auto provider = std::make_unique<mt5bridge_test_support::FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                empty_batch(operation_account, window),
                empty_batch(operation_account, window),
                settled_batch(operation_account, window)});
        mt5bridge::ObservationCoordinator coordinator(*provider, operation_account);
        mt5bridge::ObservationCollectionRequest collection;
        collection.observe_active_orders = true;
        collection.observe_positions = true;
        collection.position_history_order =
            mt5bridge::PositionHistoryOrder::positions_before_history_deals;
        collection.history_deals_window = window;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample,
                "initial observation baseline was not accepted");

        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        consistency_request.history_deals_window = window;
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, consistency_request);
        require(consistency.consistent() && consistency.proof,
                "initial environment proof was not established");

        mt5bridge::WindowsFileJournalStore store(root / "journal");
        mt5bridge::WindowsSingleWriterLease lease(root / "journal", operation_account);
        require(store.ready() && lease.ready(), "durable store or lease is not ready");
        mt5bridge::OperationJournal journal(store);
        mt5bridge::DispatchAdmissionBarrier admission(journal, coordinator.graph(), {});
        mt5bridge::runtime::OneShotDispatchBackend backend;
        mt5bridge::runtime::Mt5PythonAccountProbe account_probe(module);
        mt5bridge::runtime::Mt5PythonDispatchTransport transport(module, account_probe);
        const auto probed_account = account_probe.current_account();
        require(probed_account && *probed_account == operation_account,
                "Python account_info did not decode to the operation account");

        mt5bridge::managed_trade::ManagedTradeState initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &initial, mt5bridge::managed_trade::TradeId{7}, 1, 1, 2),
                "managed state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner owner(
            initial, operation_account, journal, admission, backend, account_probe, lease,
            transport);
        const std::string request =
            R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
        mt5bridge::dispatch::ManagedTradeIntent intent{
            std::vector<std::uint8_t>(request.begin(), request.end()),
            descriptor(operation_account, coordinator.capture_baseline(), window)};
        const auto prepared = owner.prepare_open(1, std::move(intent));
        require(prepared.status == mt5bridge::dispatch::OwnerStepStatus::prepared,
                "owner did not durably prepare OPEN");
        mt5bridge::DispatchAdmissionRequest ready;
        ready.current_account = operation_account;
        ready.environment_proof = consistency.proof;
        const auto executed = owner.execute_pending(ready);
        require(executed.status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    executed.execution_status ==
                        mt5bridge::runtime::OneShotExecutionStatus::completed &&
                    executed.retcode == 10009,
                "real Python transport did not complete the durable OPEN");

        mt5bridge::OperationReconciliationWorker worker(
            journal, *executed.key, coordinator, collection);
        const auto settled = owner.settle_reconciliation(worker);
        require(settled.status == mt5bridge::dispatch::OwnerStepStatus::completed &&
                    owner.state().open_volume == 1,
                "authoritative history evidence did not settle OPEN");
        check_order_calls(module);

        mt5bridge::OperationJournal recovered_journal(store);
        const auto recovery = recovered_journal.recover_all();
        require(recovery.accepted(), "durable restart scan failed");
        mt5bridge::managed_trade::ManagedTradeState empty;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &empty, mt5bridge::managed_trade::TradeId{7}, 1, 1, 2),
                "restart state initialization failed");
        const auto recovered = mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
            empty, operation_account, recovery);
        require(recovered && recovered->open_volume == 1 &&
                    recovered->slice.state ==
                        mt5bridge::managed_trade::OperationState::filled,
                "restart reconstruction lost settled OPEN");
        check_order_calls(module);

        }
        Py_DECREF(module);
        Py_FinalizeEx();
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (Py_IsInitialized())
            Py_FinalizeEx();
        std::filesystem::remove_all(root);
        return 1;
    }
}

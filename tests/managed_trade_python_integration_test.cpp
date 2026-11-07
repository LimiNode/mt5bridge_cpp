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
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::filesystem::path create_test_root(const char *label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned int attempt = 0; attempt != 100; ++attempt) {
        const auto root = base / (std::string(label) + "-" + std::to_string(stamp) +
                                  "-" + std::to_string(thread_id) + "-" +
                                  std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directories(root, error) && !error)
            return root;
    }
    throw std::runtime_error("could not create unique test-owned directory");
}

class TestRoot final {
public:
    explicit TestRoot(const char *label) : path_(create_test_root(label)) {}
    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TestRoot(const TestRoot &) = delete;
    TestRoot &operator=(const TestRoot &) = delete;
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

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

void set_environment(const char *name, const std::filesystem::path &value) {
#if defined(_WIN32)
    require(_putenv_s(name, value.string().c_str()) == 0,
            "could not set child-process environment");
#else
    require(setenv(name, value.string().c_str(), 1) == 0,
            "could not set child-process environment");
#endif
}

void set_environment(const char *name, const char *value) {
#if defined(_WIN32)
    require(_putenv_s(name, value) == 0,
            "could not set child-process environment");
#else
    require(setenv(name, value, 1) == 0,
            "could not set child-process environment");
#endif
}

void clear_environment(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

int run_uncertain_phase(const std::filesystem::path &root, bool first_phase) {
    Py_Initialize();
    require(Py_IsInitialized() != 0, "CPython initialization failed");
    PyObject *module = PyImport_ImportModule("MetaTrader5");
    require(module != nullptr, "fake MetaTrader5 module import failed");
    try {
        const auto operation_account = account();
        const mt5bridge::ObservationWindow window{1000, 3000};
        auto provider = std::make_unique<mt5bridge_test_support::FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                empty_batch(operation_account, window),
                empty_batch(operation_account, window)});
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
                "uncertain-process baseline was not accepted");
        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        consistency_request.history_deals_window = window;
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, consistency_request);
        require(consistency.consistent() && consistency.proof,
                "uncertain-process environment proof was not established");

        mt5bridge::WindowsFileJournalStore store(root / "journal");
        mt5bridge::WindowsSingleWriterLease lease(root / "journal", operation_account);
        require(store.ready() && lease.ready(), "uncertain-process store is not ready");
        mt5bridge::OperationJournal journal(store);
        mt5bridge::DispatchAdmissionBarrier admission(journal, coordinator.graph(), {});
        mt5bridge::runtime::OneShotDispatchBackend backend;
        mt5bridge::runtime::Mt5PythonAccountProbe account_probe(module);
        mt5bridge::runtime::Mt5PythonDispatchTransport transport(module, account_probe);
        mt5bridge::managed_trade::ManagedTradeState initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &initial, mt5bridge::managed_trade::TradeId{7}, 1, 1, 2),
                "uncertain-process state initialization failed");

        if (first_phase) {
            mt5bridge::dispatch::ManagedTradeOwner owner(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport);
            const std::string request =
                R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
            mt5bridge::dispatch::ManagedTradeIntent intent{
                std::vector<std::uint8_t>(request.begin(), request.end()),
                descriptor(operation_account, coordinator.capture_baseline(), window)};
            intent.reconciliation_descriptor.predicates = {
                mt5bridge::require_active_order(7001),
                mt5bridge::require_history_deal(7002, window)};
            const auto prepared = owner.prepare_open(1, std::move(intent));
            require(prepared.status ==
                        mt5bridge::dispatch::OwnerStepStatus::prepared,
                    "uncertain process did not prepare OPEN");
            mt5bridge::DispatchAdmissionRequest ready;
            ready.current_account = operation_account;
            ready.environment_proof = consistency.proof;
            const auto executed = owner.execute_pending(ready);
            require(executed.status == mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                        executed.record &&
                        executed.record->journal_state ==
                            mt5bridge::JournalState::reconciling &&
                        executed.record->operation_state ==
                            mt5bridge::OperationState::submitting,
                    "transport uncertainty did not become reconcile-only");
        } else {
            const auto recovery = journal.recover_all();
            require(recovery.accepted() && recovery.records().size() == 1,
                    "restart did not recover one unresolved operation");
            const auto &record = recovery.records().front();
            require(record.journal_state == mt5bridge::JournalState::reconciling &&
                        record.operation_state == mt5bridge::OperationState::submitting,
                    "unresolved operation was not kept reconciliation-only");
            require(mt5bridge::OperationRecoveryCoordinator::classify(record) ==
                        mt5bridge::OperationRecoveryAction::reconcile_only,
                    "recovery classified an unresolved operation as resendable");
            require(!mt5bridge::dispatch::ManagedTradeOwner::recover_settled_trade(
                         initial, operation_account, recovery),
                    "unresolved operation was incorrectly treated as settled");
            mt5bridge::dispatch::ManagedTradeOwner owner(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport);
            mt5bridge::DispatchAdmissionRequest ready;
            ready.current_account = operation_account;
            ready.environment_proof = consistency.proof;
            require(owner.execute_pending(ready).status ==
                        mt5bridge::dispatch::OwnerStepStatus::invalid_state,
                    "fresh owner attempted to resend an unresolved operation");
        }
        Py_DECREF(module);
        Py_FinalizeEx();
        return 0;
    } catch (...) {
        Py_DECREF(module);
        if (Py_IsInitialized())
            Py_FinalizeEx();
        throw;
    }
}

int run_uncertain_restart(const char *executable) {
    TestRoot test_root("mt5bridge-managed-trade-python-uncertain");
    const auto count_file = test_root.path() / "order-calls.txt";
    set_environment("MT5BRIDGE_CALL_COUNT_FILE", count_file);
    set_environment("MT5BRIDGE_ORDER_SEND_MODE", "none");
    const auto root_text = test_root.path().string();
    const std::string first_arg = "--uncertain-child=first=" + root_text;
    const std::string second_arg = "--uncertain-child=second=" + root_text;
    const char *first_args[] = {executable, first_arg.c_str(), nullptr};
    const char *second_args[] = {executable, second_arg.c_str(), nullptr};
#if defined(_WIN32)
    require(_spawnv(_P_WAIT, executable, first_args) == 0,
            "uncertain first process failed");
    require(_spawnv(_P_WAIT, executable, second_args) == 0,
            "uncertain second process failed");
#else
    require(std::system((std::string(executable) + " \"" + first_arg + "\"").c_str()) ==
                0,
            "uncertain first process failed");
    require(std::system((std::string(executable) + " \"" + second_arg + "\"").c_str()) ==
                0,
            "uncertain second process failed");
#endif
    std::ifstream calls(count_file);
    std::string count;
    std::getline(calls, count);
    require(count == "1", "restart path performed a duplicate order_send");
    clear_environment("MT5BRIDGE_CALL_COUNT_FILE");
    clear_environment("MT5BRIDGE_ORDER_SEND_MODE");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]).rfind("--uncertain-child=", 0) == 0) {
        const std::string argument(argv[1]);
        const auto prefix = std::string("--uncertain-child=");
        const auto phase_start = prefix.size();
        const auto separator = argument.find('=', phase_start);
        const auto phase = argument.substr(phase_start, separator - phase_start);
        const auto root = std::filesystem::path(argument.substr(separator + 1));
        return run_uncertain_phase(root, phase == "first");
    }

    if (argc == 2 && std::string(argv[1]) == "--uncertain-restart")
        return run_uncertain_restart(argv[0]);

    TestRoot test_root("mt5bridge-managed-trade-python-integration");
    const auto &root = test_root.path();
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
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (Py_IsInitialized())
            Py_FinalizeEx();
        return 1;
    }
}

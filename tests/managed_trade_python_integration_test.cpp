/// \file managed_trade_python_integration_test.cpp
/// \brief Exercises the private owner loop through the real Python transport.

#include "managed_trade_owner.hpp"
#include "python_dispatch_transport.hpp"
#include "private_owner_loop.hpp"
#include "private_trade_manager.hpp"

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
                             mt5bridge::ObservationDomain::history_orders |
                             mt5bridge::ObservationDomain::history_deals;
    batch.position_history_order =
        mt5bridge::PositionHistoryOrder::positions_before_history_deals;
    batch.history_deals_window = window;
    batch.history_orders_window = window;
    return batch;
}

mt5bridge::ObservationBatch settled_batch(const mt5bridge::AccountKey &key,
                                          mt5bridge::ObservationWindow window,
                                          double volume = 1.0) {
    auto batch = empty_batch(key, window);

    // A market OPEN may already have left the active-order view by the time
    // reconciliation runs.  The durable history-order/deal pair is the
    // authoritative identity evidence for this integration fixture.
    Mt5HistoryOrderSnapshot order{};
    order.ticket = 7001;
    order.position_id = 9001;
    order.time_done_msc = 2000;
    order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID |
                         MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
    batch.history_orders.push_back(order);

    Mt5DealSnapshot deal{};
    deal.ticket = 7002;
    deal.order_ticket = 7001;
    deal.position_id = 9001;
    deal.entry = 0;
    deal.volume = volume;
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

mt5bridge::ObservationBatch late_fill_batch(
    const mt5bridge::AccountKey &key, mt5bridge::ObservationWindow window) {
    auto batch = settled_batch(key, window);
    auto deal = batch.history_deals.front();
    deal.ticket = 7003;
    deal.time_msc = 2100;
    batch.history_deals.push_back(deal);
    return batch;
}

mt5bridge::ReconciliationDescriptor descriptor(
    const mt5bridge::AccountKey &key, const mt5bridge::ReconciliationBaseline &baseline,
    mt5bridge::ObservationWindow window) {
    return {key,
            baseline,
            {mt5bridge::expect_reconciliation_transition(
                 mt5bridge::ReconciliationPredicateKind::history_order_present,
                 std::nullopt, false,
                 mt5bridge::ReconciliationTransition::absent_to_present, 1001,
                 window),
             mt5bridge::expect_reconciliation_transition(
                 mt5bridge::ReconciliationPredicateKind::history_deal_present,
                 std::nullopt, false,
                 mt5bridge::ReconciliationTransition::absent_to_present, 1002,
                 window)},
            mt5bridge::OperationState::filled, 7, 1};
}

void require_bindings(const mt5bridge::OperationRecord &record) {
    require(record.reconciliation_bindings.size() == 2,
            "broker result bindings were not durably captured");
    require(record.reconciliation_bindings[0] ==
                mt5bridge::ReconciliationBinding{1001, 7001} &&
                record.reconciliation_bindings[1] ==
                    mt5bridge::ReconciliationBinding{1002, 7002},
            "durable broker bindings did not match result tickets");
}

void check_order_calls(PyObject *module) {
    mt5bridge::runtime::PyRef calls(PyObject_GetAttrString(module, "order_calls"));
    require(calls && PyLong_Check(calls.get()) && PyLong_AsLong(calls.get()) == 1,
            "Python adapter did not perform exactly one order_send call");
}

void require_order_calls(PyObject *module, long expected) {
    mt5bridge::runtime::PyRef calls(PyObject_GetAttrString(module, "order_calls"));
    require(calls && PyLong_Check(calls.get()) && PyLong_AsLong(calls.get()) == expected,
            "Python adapter order_send count did not match expectation");
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

std::string environment_value(const char *name) {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value)
        return {};
    std::string result(value, length ? length - 1 : 0);
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string{};
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
        collection.history_orders_window = window;
        collection.history_deals_window = window;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample,
                "uncertain-process baseline was not accepted");
        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        consistency_request.history_orders_window = window;
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
            // This separate no-result case retains fixed identities so its
            // transport-uncertainty assertion is independent of result binding.
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
            const auto journal_recovery = journal.recover_all();
            const auto recovery = mt5bridge::OperationRecoveryCoordinator::recover(journal);
            require(recovery.accepted() && recovery.operations.size() == 1,
                    "restart did not recover one unresolved operation");
            const auto &recovered = recovery.operations.front();
            const auto &record = recovered.record;
            require(record.journal_state == mt5bridge::JournalState::reconciling &&
                        record.operation_state == mt5bridge::OperationState::submitting,
                    "unresolved operation was not kept reconciliation-only");
            require(recovered.action ==
                        mt5bridge::OperationRecoveryAction::reconcile_only,
                    "recovery classified an unresolved operation as resendable");
            mt5bridge::DispatchAdmissionRequest admission_request;
            admission_request.current_account = operation_account;
            admission_request.environment_proof = consistency.proof;
            const auto admission_result = admission.admit(
                record.key, admission_request, lease);
            require(!admission_result.admitted() &&
                        admission_result.status ==
                            mt5bridge::DispatchAdmissionStatus::invalid_state &&
                        !admission_result.permit,
                    "recovered dispatch barrier was reopened");
            const auto after_admission = journal.find(record.key);
            require(after_admission &&
                        after_admission->journal_state ==
                            mt5bridge::JournalState::reconciling,
                    "recovery admission changed the durable journal state");
            require(!mt5bridge::dispatch::ManagedTradeOwner::recover_settled_trade(
                         initial, operation_account, journal_recovery),
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
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        Py_DECREF(module);
        if (Py_IsInitialized())
            Py_FinalizeEx();
        return 1;
    }
}

int run_uncertain_restart(const char *executable) {
    TestRoot test_root("mt5bridge-managed-trade-python-uncertain");
    const auto count_file = test_root.path() / "order-calls.txt";
    set_environment("MT5BRIDGE_CALL_COUNT_FILE", count_file);
    set_environment("MT5BRIDGE_ORDER_SEND_MODE", "none");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "0");
    const auto root_text = test_root.path().string();
    const std::string first_arg = "--uncertain-child=first=" + root_text;
    const std::string second_arg = "--uncertain-child=second=" + root_text;
    const char *first_args[] = {executable, first_arg.c_str(), nullptr};
    const char *second_args[] = {executable, second_arg.c_str(), nullptr};
#if defined(_WIN32)
    require(_spawnv(_P_WAIT, executable, first_args) == 0,
            "uncertain first process failed");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "1");
    require(_spawnv(_P_WAIT, executable, second_args) == 0,
            "uncertain second process failed");
#else
    require(std::system((std::string(executable) + " \"" + first_arg + "\"").c_str()) ==
                0,
            "uncertain first process failed");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "1");
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
    clear_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT");
    return 0;
}

int run_open_restart_phase(const std::filesystem::path &root,
                           const std::string &phase) {
    Py_Initialize();
    require(Py_IsInitialized() != 0, "CPython initialization failed");
    PyObject *module = PyImport_ImportModule("MetaTrader5");
    require(module != nullptr, "fake MetaTrader5 module import failed");
    try {
        const bool first_phase = phase == "first";
        const auto selected = environment_value("MT5BRIDGE_RESTART_SCENARIO");
        const std::string scenario = selected.empty() ? "immediate" : selected;
        const auto operation_account = account();
        const mt5bridge::ObservationWindow window{1000, 3000};
        std::vector<mt5bridge::ObservationBatch> batches;
        if (first_phase) {
            batches = {empty_batch(operation_account, window),
                       empty_batch(operation_account, window)};
        } else if (scenario == "prepared") {
            auto settled = settled_batch(operation_account, window);
            batches = {empty_batch(operation_account, window),
                       empty_batch(operation_account, window), std::move(settled)};
        } else if (scenario == "delayed") {
            batches = {empty_batch(operation_account, window),
                       settled_batch(operation_account, window)};
        } else if (scenario == "missing") {
            batches = {empty_batch(operation_account, window)};
        } else if (scenario == "wrong_ticket") {
            auto wrong = settled_batch(operation_account, window);
            wrong.history_deals.front().ticket = 7999;
            batches = {std::move(wrong)};
        } else if (scenario == "wrong_order") {
            auto wrong = settled_batch(operation_account, window);
            wrong.history_deals.front().order_ticket = 7999;
            batches = {std::move(wrong)};
        } else if (scenario == "wrong_account") {
            batches = {settled_batch({"Other-Server", 99}, window)};
        } else if (scenario == "incomplete") {
            batches = {settled_batch(operation_account, {1500, 2500})};
        } else if (scenario == "partial") {
            batches = {settled_batch(operation_account, window),
                       late_fill_batch(operation_account, window)};
        } else if (scenario == "third") {
            batches = {};
        } else {
            batches = {settled_batch(operation_account, window)};
        }
        auto provider = std::make_unique<mt5bridge_test_support::FakeObservationProvider>(
            std::move(batches));
        mt5bridge::ObservationCoordinator coordinator(*provider, operation_account);
        mt5bridge::ObservationCollectionRequest collection;
        collection.observe_active_orders = true;
        collection.observe_positions = true;
        collection.position_history_order =
            mt5bridge::PositionHistoryOrder::positions_before_history_deals;
        collection.history_orders_window = window;
        collection.history_deals_window = window;

        mt5bridge::WindowsFileJournalStore store(root / "journal");
        mt5bridge::WindowsSingleWriterLease lease(root / "journal", operation_account);
        require(store.ready() && lease.ready(), "open-restart store is not ready");
        mt5bridge::OperationJournal journal(store);
        mt5bridge::runtime::OneShotDispatchBackend backend;
        mt5bridge::runtime::Mt5PythonAccountProbe account_probe(module);
        mt5bridge::runtime::Mt5PythonDispatchTransport transport(module, account_probe);

        mt5bridge::managed_trade::ManagedTradeState initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &initial, mt5bridge::managed_trade::TradeId{7}, 2, 2, 2),
                "open-restart state initialization failed");

        if (first_phase) {
            const auto first = coordinator.refresh(collection);
            const auto second = coordinator.refresh(collection);
            require(first.sample && second.sample,
                    "open-restart baseline was not accepted");
            mt5bridge::EnvironmentConsistencyRequest consistency_request;
            consistency_request.history_orders_window = window;
            consistency_request.history_deals_window = window;
            const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*first.sample, *second.sample}, consistency_request);
            require(consistency.consistent() && consistency.proof,
                    "open-restart environment proof was not established");

            mt5bridge::DispatchAdmissionBarrier admission(
                journal, coordinator.graph(), {});
            if (scenario == "prepared") {
                mt5bridge::dispatch::PrivateTradeManager manager(
                    initial, operation_account, journal, admission, backend,
                    account_probe, lease, transport, coordinator, collection);
                const std::string request =
                    R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
                mt5bridge::dispatch::ManagedTradeIntent intent{
                    std::vector<std::uint8_t>(request.begin(), request.end()),
                    descriptor(operation_account, coordinator.capture_baseline(), window)};
                require(manager.start().status ==
                            mt5bridge::dispatch::PrivateTradeManagerStatus::ready,
                        "prepared first process did not start manager");
                const auto prepared = manager.prepare_open(1, std::move(intent));
                require(prepared.owner &&
                            prepared.owner->status ==
                                mt5bridge::dispatch::OwnerStepStatus::prepared,
                        "prepared first process did not persist OPEN intent");
                require(manager.state() && manager.state()->open_volume == 0,
                        "prepared first process fabricated exposure");
                require_order_calls(module, 0);
                Py_DECREF(module);
                Py_FinalizeEx();
                return 0;
            }
            mt5bridge::dispatch::ManagedTradeOwner owner(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport);
            const std::string request =
                scenario == "partial"
                    ? R"({"action":1,"type":0,"symbol":"EURUSD","volume":2.0})"
                    : R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
            mt5bridge::dispatch::ManagedTradeIntent intent{
                std::vector<std::uint8_t>(request.begin(), request.end()),
                descriptor(operation_account, coordinator.capture_baseline(), window)};
            const auto prepared = owner.prepare_open(
                scenario == "partial" ? 2 : 1, std::move(intent));
            require(prepared.status ==
                        mt5bridge::dispatch::OwnerStepStatus::prepared,
                    "open-restart first process did not prepare OPEN");
            mt5bridge::DispatchAdmissionRequest ready;
            ready.current_account = operation_account;
            ready.environment_proof = consistency.proof;
            const auto executed = owner.execute_pending(ready);
            require(executed.status ==
                        mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                        executed.record &&
                        executed.record->journal_state ==
                            mt5bridge::JournalState::result_persisted &&
                        executed.record->operation_state ==
                            mt5bridge::OperationState::reconciling,
                    "open-restart first process did not persist unresolved result");
            require(executed.record.has_value(),
                    "open-restart did not persist a result-bearing record");
            require_bindings(*executed.record);
            require(owner.state().open_volume == 0 &&
                        owner.state().slice.state ==
                            mt5bridge::managed_trade::OperationState::submitting,
                    "open-restart first process fabricated exposure");
        } else if (phase == "third") {
            const auto recovery = journal.recover_all();
            require(recovery.accepted(), "third-process journal scan failed");
            const auto durable = journal.find({operation_account, 7, 1});
            require(durable.has_value(), "third-process OPEN record missing");
            require_bindings(*durable);
            require(durable->operation_state == mt5bridge::OperationState::filled &&
                        durable->settled_volume == 1,
                    "third-process replay changed settled OPEN");
            const auto replayed =
                mt5bridge::dispatch::ManagedTradeOwner::recover_settled_open(
                    initial, operation_account, recovery);
            require(replayed && replayed->open_volume == 1,
                    "third-process replay duplicated or lost OPEN exposure");
            mt5bridge::DispatchAdmissionBarrier admission(
                journal, coordinator.graph(), {});
            mt5bridge::dispatch::PrivateTradeManager manager(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport, coordinator, collection);
            require(manager.start().status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::ready &&
                        manager.state() && manager.state()->open_volume == 1,
                    "third-process manager did not restore settled exposure");
            mt5bridge::DispatchAdmissionRequest resend;
            resend.current_account = operation_account;
            require(manager.execute_pending(resend).status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::blocked,
                    "third-process manager reopened settled dispatch");
            check_order_calls(module);
        } else if (scenario == "prepared") {
            const auto first = coordinator.refresh(collection);
            const auto second = coordinator.refresh(collection);
            require(first.sample && second.sample,
                    "prepared second process did not collect fresh baseline");
            mt5bridge::EnvironmentConsistencyRequest consistency_request;
            consistency_request.history_orders_window = window;
            consistency_request.history_deals_window = window;
            const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
                {*first.sample, *second.sample}, consistency_request);
            require(consistency.consistent() && consistency.proof,
                    "prepared second process did not establish environment proof");
            mt5bridge::DispatchAdmissionBarrier admission(
                journal, coordinator.graph(), {});
            mt5bridge::dispatch::PrivateTradeManager manager(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport, coordinator, collection);
            require(manager.start().status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::ready &&
                        manager.state() &&
                        manager.state()->slice.state ==
                            mt5bridge::managed_trade::OperationState::dispatching,
                    "prepared second process did not restore OPEN intent");
            mt5bridge::DispatchAdmissionRequest ready;
            ready.current_account = operation_account;
            ready.environment_proof = consistency.proof;
            const auto executed = manager.execute_pending(ready);
            require(executed.owner &&
                        executed.owner->status ==
                            mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation,
                    "prepared second process did not execute OPEN");
            const auto settled = manager.reconcile();
            require(settled.owner &&
                        settled.owner->status ==
                            mt5bridge::dispatch::OwnerStepStatus::completed &&
                        manager.state() && manager.state()->open_volume == 1,
                    "prepared second process did not reconcile OPEN");
            check_order_calls(module);
        } else {
            mt5bridge::dispatch::PrivateOwnerLoop loop(
                journal, operation_account, lease);
            const auto startup = loop.start();
            require(startup.ready() && startup.operation_count == 1,
                    "open-restart second process did not complete owner startup");
            const auto action = loop.step();
            require(action.status ==
                        mt5bridge::dispatch::PrivateOwnerLoopStepStatus::reconcile_only &&
                        action.operation,
                    "open-restart recovery became resendable");

            const auto recovery = journal.recover_all();
            require(recovery.accepted(), "open-restart journal scan failed");
            const auto recovered =
                mt5bridge::dispatch::ManagedTradeOwner::recover_pending_open(
                    initial, operation_account, recovery);
            require(recovered && recovered->open_volume == 0 &&
                        recovered->slice.state ==
                            mt5bridge::managed_trade::OperationState::submitting,
                    "open-restart did not rebuild the unresolved managed slice");
            const auto durable = journal.find({operation_account, 7, 1});
            require(durable.has_value(),
                    "restart did not restore broker result bindings");
            require_bindings(*durable);
            require(durable->reconciliation_descriptor &&
                        durable->reconciliation_descriptor->predicates[0].ticket == 0 &&
                        durable->reconciliation_descriptor->predicates[1].ticket == 0,
                    "pre-dispatch descriptor contained a broker ticket");

            mt5bridge::DispatchAdmissionBarrier admission(
                journal, coordinator.graph(), {});
            mt5bridge::dispatch::PrivateTradeManager manager(
                initial, operation_account, journal, admission, backend, account_probe,
                lease, transport, coordinator, collection);
            require(manager.start().status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::ready &&
                        manager.state() && manager.state()->open_volume == 0,
                    "private manager did not reconstruct unresolved OPEN");
            mt5bridge::DispatchAdmissionRequest resend;
            resend.current_account = operation_account;
            require(manager.execute_pending(resend).status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::blocked,
                    "restarted manager tried to reopen durable dispatch");
            const auto settled = manager.reconcile();
            require(settled.owner.has_value(),
                    "restarted manager did not return reconciliation outcome");
            if (scenario == "missing" || scenario == "wrong_ticket" ||
                scenario == "wrong_order" || scenario == "wrong_account" ||
                scenario == "incomplete") {
                require(settled.owner->status !=
                            mt5bridge::dispatch::OwnerStepStatus::completed &&
                            manager.state() && manager.state()->open_volume == 0,
                        "foreign or incomplete evidence settled OPEN");
                const auto unchanged = journal.find({operation_account, 7, 1});
                require(unchanged && unchanged->settled_volume == 0,
                        "negative evidence changed durable settled volume");
            } else if (scenario == "delayed") {
                require(settled.owner->status ==
                            mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                            manager.state() && manager.state()->open_volume == 0,
                        "missing deal did not keep restarted OPEN unresolved");
                const auto later = manager.reconcile();
                require(later.owner &&
                            later.owner->status ==
                                mt5bridge::dispatch::OwnerStepStatus::completed &&
                            later.owner->record &&
                            later.owner->record->settled_volume == 1 &&
                            manager.state() && manager.state()->open_volume == 1,
                        "late matching deal did not settle OPEN");
            } else if (scenario == "partial") {
                require(settled.owner->status ==
                            mt5bridge::dispatch::OwnerStepStatus::partially_filled &&
                            settled.owner->record &&
                            settled.owner->record->settled_volume == 1 &&
                            manager.state() && manager.state()->open_volume == 1 &&
                            manager.state()->pending_remainder_volume == 1,
                        "partial fill did not preserve confirmed volume and remainder");
                const auto later = manager.reconcile();
                require(later.owner &&
                            later.owner->status ==
                                mt5bridge::dispatch::OwnerStepStatus::completed &&
                            later.owner->record &&
                            later.owner->record->settled_volume == 2 &&
                            manager.state() && manager.state()->open_volume == 2 &&
                            manager.state()->pending_remainder_volume == 0,
                        "late fill did not apply only the cumulative delta");
            } else {
                require(settled.owner->status ==
                        mt5bridge::dispatch::OwnerStepStatus::completed &&
                        settled.owner->record &&
                        settled.owner->record->operation_state ==
                            mt5bridge::OperationState::filled &&
                        manager.state() && manager.state()->open_volume == 1 &&
                        manager.state()->pending_remainder_volume == 0,
                    "open-restart matching deal did not settle OPEN");
            }
            check_order_calls(module);
        }
        Py_DECREF(module);
        Py_FinalizeEx();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        Py_DECREF(module);
        if (Py_IsInitialized())
            Py_FinalizeEx();
        return 1;
    }
}

int run_open_restart(const char *executable, const char *scenario = "immediate") {
    TestRoot test_root("mt5bridge-managed-trade-python-open-restart");
    const auto count_file = test_root.path() / "order-calls.txt";
    set_environment("MT5BRIDGE_CALL_COUNT_FILE", count_file);
    set_environment("MT5BRIDGE_ORDER_SEND_MODE", "success");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "0");
    set_environment("MT5BRIDGE_RESTART_SCENARIO", scenario);
    if (std::string(scenario) == "prepared") {
        std::ofstream initial_count(count_file, std::ios::trunc);
        initial_count << "0";
    }
    const auto root_text = test_root.path().string();
    const std::string first_arg = "--open-restart-child=first=" + root_text;
    const std::string second_arg = "--open-restart-child=second=" + root_text;
    const std::string third_arg = "--open-restart-child=third=" + root_text;
    const char *first_args[] = {executable, first_arg.c_str(), nullptr};
    const char *second_args[] = {executable, second_arg.c_str(), nullptr};
    const char *third_args[] = {executable, third_arg.c_str(), nullptr};
#if defined(_WIN32)
    const auto first_status = _spawnv(_P_WAIT, executable, first_args);
    if (first_status != 0)
        std::cerr << "open-restart first status: " << first_status << '\n';
    require(first_status == 0, "open-restart first process failed");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "1");
    const auto second_status = _spawnv(_P_WAIT, executable, second_args);
    if (second_status != 0)
        std::cerr << "open-restart second status: " << second_status << '\n';
    require(second_status == 0, "open-restart second process failed");
    if (std::string(scenario) == "immediate")
        require(_spawnv(_P_WAIT, executable, third_args) == 0,
                "open-restart third process failed");
#else
    require(std::system((std::string(executable) + " \"" + first_arg + "\"").c_str()) ==
                0,
            "open-restart first process failed");
    set_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT", "1");
    require(std::system((std::string(executable) + " \"" + second_arg + "\"").c_str()) ==
                0,
            "open-restart second process failed");
    if (std::string(scenario) == "immediate")
        require(std::system((std::string(executable) + " \"" + third_arg + "\"").c_str()) ==
                    0,
                "open-restart third process failed");
#endif
    std::ifstream calls(count_file);
    std::string count;
    std::getline(calls, count);
    require(count == "1", "open-restart performed a duplicate order_send");
    clear_environment("MT5BRIDGE_CALL_COUNT_FILE");
    clear_environment("MT5BRIDGE_ORDER_SEND_MODE");
    clear_environment("MT5BRIDGE_EXPECT_EXISTING_COUNT");
    clear_environment("MT5BRIDGE_RESTART_SCENARIO");
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

    if (argc == 2 && std::string(argv[1]) == "--open-restart")
        return run_open_restart(argv[0]);

    if (argc == 2 && std::string(argv[1]).rfind("--open-restart-scenario=", 0) == 0)
        return run_open_restart(argv[0],
                                std::string(argv[1]).substr(
                                    std::string("--open-restart-scenario=").size())
                                    .c_str());

    if (argc == 2 && std::string(argv[1]) == "--open-restart-matrix") {
        try {
            for (const auto *scenario : {"delayed", "missing", "wrong_ticket",
                                         "wrong_order", "wrong_account", "incomplete",
                                         "partial"}) {
                run_open_restart(argv[0], scenario);
            }
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    if (argc == 2 && std::string(argv[1]).rfind("--open-restart-child=", 0) == 0) {
        const std::string argument(argv[1]);
        const auto prefix = std::string("--open-restart-child=");
        const auto phase_start = prefix.size();
        const auto separator = argument.find('=', phase_start);
        const auto phase = argument.substr(phase_start, separator - phase_start);
        const auto root = std::filesystem::path(argument.substr(separator + 1));
        return run_open_restart_phase(root, phase);
    }

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
        collection.history_orders_window = window;
        collection.history_deals_window = window;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample,
                "initial observation baseline was not accepted");

        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        consistency_request.history_orders_window = window;
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
        mt5bridge::dispatch::PrivateTradeManager manager(
            initial, operation_account, journal, admission, backend, account_probe,
            lease, transport, coordinator, collection);
        require(manager.state() == nullptr &&
                    manager.start().status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::ready,
                "private manager did not complete durable startup");
        const std::string request =
            R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
        mt5bridge::dispatch::ManagedTradeIntent intent{
            std::vector<std::uint8_t>(request.begin(), request.end()),
            descriptor(operation_account, coordinator.capture_baseline(), window)};
        const auto prepared = manager.prepare_open(1, std::move(intent));
        require(prepared.owner &&
                    prepared.owner->status ==
                        mt5bridge::dispatch::OwnerStepStatus::prepared,
                "owner did not durably prepare OPEN");
        manager.shutdown();
        require(manager.start().status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::ready &&
                    manager.state() &&
                    manager.state()->slice.state ==
                        mt5bridge::managed_trade::OperationState::dispatching,
                "manager did not restore the pre-dispatch OPEN intent");
        mt5bridge::DispatchAdmissionRequest ready;
        ready.current_account = operation_account;
        ready.environment_proof = consistency.proof;
        const auto executed = manager.execute_pending(ready);
        require(executed.owner && executed.owner->status ==
                    mt5bridge::dispatch::OwnerStepStatus::awaiting_reconciliation &&
                    executed.owner->execution_status ==
                        mt5bridge::runtime::OneShotExecutionStatus::completed &&
                    executed.owner->retcode == 10009,
                "real Python transport did not complete the durable OPEN");
        require(executed.owner->record.has_value(),
                "real Python transport did not return a durable record");
        require_bindings(*executed.owner->record);
        require(manager.execute_pending(ready).status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::blocked,
                "manager reopened a durable dispatch barrier");

        const auto settled = manager.reconcile();
        require(settled.owner &&
                    settled.owner->status ==
                        mt5bridge::dispatch::OwnerStepStatus::completed &&
                    manager.state() && manager.state()->open_volume == 1,
                "authoritative history evidence did not settle OPEN");
        mt5bridge::dispatch::ManagedTradeIntent another_intent{
            std::vector<std::uint8_t>(request.begin(), request.end()),
            descriptor(operation_account, coordinator.capture_baseline(), window)};
        require(manager.prepare_open(1, std::move(another_intent)).status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::invalid_state,
                "single-slice manager admitted a second OPEN");
        manager.shutdown();
        require(manager.state() == nullptr &&
                    manager.execute_pending(ready).status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::stopped,
                "shutdown retained an executable owner");
        check_order_calls(module);

        mt5bridge::OperationJournal recovered_journal(store);
        const auto recovery = recovered_journal.recover_all();
        require(recovery.accepted(), "durable restart scan failed");
        const auto durable = recovered_journal.find({operation_account, 7, 1});
        require(durable.has_value(), "durable restart lost the OPEN record");
        require_bindings(*durable);
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

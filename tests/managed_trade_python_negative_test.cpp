/// \file managed_trade_python_negative_test.cpp
/// \brief Exercises fail-closed Python-backed owner boundaries.

#include "managed_trade_owner.hpp"
#include "python_dispatch_transport.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>
#include <mt5bridge/reconciliation/engine.hpp>

#include "support/fake_observation_provider.hpp"

#include <Python.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TestRoot final {
public:
    explicit TestRoot(const char *label) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned int attempt = 0; attempt != 100; ++attempt) {
            path_ = base / (std::string(label) + "-" + std::to_string(stamp) + "-" +
                            std::to_string(thread_id) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directories(path_, error) && !error)
                return;
        }
        throw std::runtime_error("could not create negative-test directory");
    }

    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

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
    batch.position_history_order = mt5bridge::PositionHistoryOrder::positions_before_history_deals;
    batch.history_deals_window = window;
    return batch;
}

mt5bridge::ReconciliationDescriptor descriptor(
    const mt5bridge::AccountKey &key, const mt5bridge::ReconciliationBaseline &baseline,
    mt5bridge::ObservationWindow window) {
    return {key,
            baseline,
            {mt5bridge::require_active_order(7001),
             mt5bridge::require_history_deal(7002, window)},
            mt5bridge::OperationState::filled, 7, 1};
}

void set_environment(const char *name, const std::string &value) {
#if defined(_WIN32)
    require(_putenv_s(name, value.c_str()) == 0, "could not set negative-test environment");
#else
    require(setenv(name, value.c_str(), 1) == 0, "could not set negative-test environment");
#endif
}

void clear_environment(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

enum class NegativeMode { account_mismatch, none, exception, malformed, market_closed };

const char *mode_name(NegativeMode mode) {
    switch (mode) {
    case NegativeMode::account_mismatch:
        return "account-mismatch";
    case NegativeMode::none:
        return "none";
    case NegativeMode::exception:
        return "exception";
    case NegativeMode::malformed:
        return "malformed";
    case NegativeMode::market_closed:
        return "market-closed";
    }
    return "unknown";
}

void run_case(NegativeMode mode) {
    TestRoot root((std::string("mt5bridge-python-negative-") + mode_name(mode)).c_str());
    const auto count_file = root.path() / "order-calls.txt";
    const auto operation_account = account();
    const auto observed_account = mode == NegativeMode::account_mismatch
                                      ? mt5bridge::AccountKey{"Other-Server", 99}
                                      : operation_account;
    set_environment("MT5BRIDGE_CALL_COUNT_FILE", count_file.string());
    set_environment("MT5BRIDGE_ORDER_SEND_MODE",
                    mode == NegativeMode::account_mismatch
                        ? "success"
                        : (mode == NegativeMode::market_closed ? "market_closed"
                                                                : mode_name(mode)));
    set_environment("MT5BRIDGE_ACCOUNT_SERVER", observed_account.server);
    set_environment("MT5BRIDGE_ACCOUNT_LOGIN", std::to_string(observed_account.login));

    Py_Initialize();
    require(Py_IsInitialized() != 0, "CPython initialization failed");
    PyObject *module = PyImport_ImportModule("MetaTrader5");
    require(module != nullptr, "fake MetaTrader5 module import failed");
    try {
        {
        const mt5bridge::ObservationWindow window{1000, 3000};
        auto provider = std::make_unique<mt5bridge_test_support::FakeObservationProvider>(
            std::vector<mt5bridge::ObservationBatch>{
                empty_batch(operation_account, window), empty_batch(operation_account, window)});
        mt5bridge::ObservationCoordinator coordinator(*provider, operation_account);
        mt5bridge::ObservationCollectionRequest collection;
        collection.observe_active_orders = true;
        collection.observe_positions = true;
        collection.position_history_order =
            mt5bridge::PositionHistoryOrder::positions_before_history_deals;
        collection.history_deals_window = window;
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample, "negative-test baseline was not accepted");
        mt5bridge::EnvironmentConsistencyRequest consistency_request;
        consistency_request.history_deals_window = window;
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, consistency_request);
        require(consistency.consistent() && consistency.proof,
                "negative-test environment proof was not established");

        mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
        mt5bridge::WindowsSingleWriterLease lease(root.path() / "journal", operation_account);
        require(store.ready() && lease.ready(), "negative-test store or lease is not ready");
        mt5bridge::OperationJournal journal(store);
        mt5bridge::DispatchAdmissionBarrier admission(journal, coordinator.graph(), {});
        mt5bridge::runtime::OneShotDispatchBackend backend;
        mt5bridge::runtime::Mt5PythonAccountProbe account_probe(module);
        mt5bridge::runtime::Mt5PythonDispatchTransport transport(module, account_probe);
        mt5bridge::managed_trade::ManagedTradeState initial;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &initial, mt5bridge::managed_trade::TradeId{7}, 1, 1, 2),
                "negative-test state initialization failed");
        mt5bridge::dispatch::ManagedTradeOwner owner(
            initial, operation_account, journal, admission, backend, account_probe, lease,
            transport);
        const std::string request =
            R"({"action":1,"type":0,"symbol":"EURUSD","volume":1.0})";
        mt5bridge::dispatch::ManagedTradeIntent intent{
            std::vector<std::uint8_t>(request.begin(), request.end()),
            descriptor(operation_account, coordinator.capture_baseline(), window)};
        require(owner.prepare_open(1, std::move(intent)).status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "negative-test OPEN was not prepared");
        mt5bridge::DispatchAdmissionRequest ready;
        ready.current_account = operation_account;
        ready.environment_proof = consistency.proof;
        const auto executed = owner.execute_pending(ready);
        const auto record = executed.key ? journal.find(*executed.key) : std::nullopt;
        require(record.has_value(), "negative-test lost durable operation record");

        std::ifstream calls(count_file);
        std::string call_count;
        std::getline(calls, call_count);
        if (mode == NegativeMode::account_mismatch) {
            require(executed.status == mt5bridge::dispatch::OwnerStepStatus::execution_failed &&
                        executed.execution_status ==
                            mt5bridge::runtime::OneShotExecutionStatus::account_mismatch &&
                        record->journal_state == mt5bridge::JournalState::dispatching &&
                        record->operation_state == mt5bridge::OperationState::prechecking,
                    "account mismatch was not rejected before dispatch");
            require(call_count.empty(),
                    "account mismatch unexpectedly reached Python order_send");
        } else if (mode == NegativeMode::market_closed) {
            require(executed.status == mt5bridge::dispatch::OwnerStepStatus::completed &&
                        executed.retcode == 10018 &&
                        record->journal_state == mt5bridge::JournalState::result_persisted &&
                        record->operation_state == mt5bridge::OperationState::rejected,
                    "MARKET_CLOSED was not durably rejected");
            require(call_count == "1", "MARKET_CLOSED send count is not exactly one");
        } else {
            require(executed.status == mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                        record->journal_state == mt5bridge::JournalState::reconciling &&
                        record->operation_state == mt5bridge::OperationState::submitting,
                    "negative transport result did not become reconcile-only");
            require(call_count == "1", "negative transport call count was unexpected");
        }

        }
        Py_DECREF(module);
        module = nullptr;
        Py_FinalizeEx();
    } catch (...) {
        if (module)
            Py_DECREF(module);
        if (Py_IsInitialized())
            Py_FinalizeEx();
        throw;
    }
    clear_environment("MT5BRIDGE_CALL_COUNT_FILE");
    clear_environment("MT5BRIDGE_ORDER_SEND_MODE");
    clear_environment("MT5BRIDGE_ACCOUNT_SERVER");
    clear_environment("MT5BRIDGE_ACCOUNT_LOGIN");
}

} // namespace

int main() {
    try {
        run_case(NegativeMode::account_mismatch);
        run_case(NegativeMode::none);
        run_case(NegativeMode::exception);
        run_case(NegativeMode::malformed);
        run_case(NegativeMode::market_closed);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (Py_IsInitialized())
            Py_FinalizeEx();
        return 1;
    }
}

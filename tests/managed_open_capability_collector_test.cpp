/// \file managed_open_capability_collector_test.cpp
/// \brief Verifies runtime-owned capability collection with concrete Windows leases.

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

#include "managed_open_capability_collector.hpp"
#include "python_ref.hpp"
#include <mt5bridge/abi.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::trade;
using Status = ManagedOpenCollectionStatus;

static_assert(!std::is_default_constructible_v<ManagedOpenCapabilitySnapshot>,
              "source snapshots cannot be default-created");
static_assert(!std::is_constructible_v<ManagedOpenCapabilitySnapshot,
                                      ManagedOpenCapabilityData, std::uint64_t,
                                      std::uint64_t, std::chrono::steady_clock::time_point>,
              "plain facts cannot construct a source snapshot");

/// \brief Keeps runtime checks active in Release builds.
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// \brief Owns the GIL only for configuring or inspecting the deterministic fake.
class GilScope {
public:
    /// \brief Acquires the initialized interpreter's GIL.
    GilScope() : state_(PyGILState_Ensure()) {}
    /// \brief Releases the GIL before invoking the runtime-owned collector.
    ~GilScope() { PyGILState_Release(state_); }
    GilScope(const GilScope &) = delete;
    GilScope &operator=(const GilScope &) = delete;

private:
    PyGILState_STATE state_;
};

/// \brief Reuses the runtime's shared strong-reference ownership primitive.
using PythonRef = mt5bridge::runtime::PyRef;

/// \brief Isolates real lease metadata and removes only its own temporary directory.
class TemporaryDirectory {
public:
    /// \brief Creates a fresh test directory without reusing another run's data.
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("mt5bridge-capability-" + std::to_string(stamp) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error))
                return;
        }
        throw std::runtime_error("cannot create isolated capability test directory");
    }
    /// \brief Removes only the directory created successfully by this instance.
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    /// \brief Returns the owned absolute metadata path.
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// \brief Owns actual DLL initialization and shutdown on the native owner thread.
class Runtime {
public:
    /// \brief Starts the real DLL-owned interpreter, without external Python setup.
    Runtime() { start(); }
    /// \brief Finalizes the interpreter when a test throws.
    ~Runtime() {
        if (running_)
            mt5bridge_shutdown();
    }
    /// \brief Starts a new owned runtime lifecycle and fake-module instance.
    void start() {
        require(!running_, "runtime already started");
        require(mt5bridge_initialize(nullptr) == 0, "owned runtime initialization failed");
        running_ = true;
    }
    /// \brief Stops the current lifecycle before generation-invalidity checks.
    void stop() {
        require(running_, "runtime not started");
        require(mt5bridge_shutdown() == 0, "owned runtime shutdown failed");
        running_ = false;
    }

private:
    bool running_ = false;
};

/// \brief Selects one deterministic source scenario under the initialized GIL.
void configure(const std::string &scenario) {
    GilScope gil;
    PythonRef module(PyImport_ImportModule("MetaTrader5"));
    require(module.get() != nullptr, "cannot import capability fake");
    PythonRef result(PyObject_CallMethod(module.get(), "configure", "s", scenario.c_str()));
    if (!result.get())
        PyErr_Print();
    require(result.get() != nullptr, "cannot configure fake scenario " + scenario);
}

/// \brief Reads one fake-module audit counter without retaining Python state.
std::uint64_t counter(const char *name) {
    GilScope gil;
    PythonRef module(PyImport_ImportModule("MetaTrader5"));
    require(module.get() != nullptr, "cannot inspect capability fake");
    PythonRef result(PyObject_CallMethod(module.get(), "stats", nullptr));
    require(result.get() && PyDict_Check(result.get()), "missing fake source audit");
    auto *item = PyDict_GetItemString(result.get(), name);
    if (!item)
        return 0;
    const auto value = PyLong_AsUnsignedLongLong(item);
    require(!PyErr_Occurred(), "invalid fake source audit counter");
    return static_cast<std::uint64_t>(value);
}

/// \brief Proves observation neither sends nor creates any operation journal record.
void require_read_only(const WindowsFileJournalStore &journal) {
    require(counter("order_send") == 0, "collector dispatched an order");
    const auto scan = journal.scan();
    require(scan.complete() && scan.records.empty(), "collector created operation records");
}

/// \brief Collects one successful source observation and checks complete repeated books.
ManagedOpenCapabilitySnapshot collect(const std::string &scenario, const AccountKey &account,
                                      const WindowsSingleWriterLease &lease,
                                      const WindowsFileJournalStore &journal) {
    configure(scenario);
    auto result = collect_managed_open_capabilities(account, "EURUSD", lease);
    require(result.status == Status::collected && result.snapshot.has_value(),
            "collection failed for " + scenario + " status=" +
                std::to_string(static_cast<int>(result.status)));
    require(counter("positions_get") == 2 && counter("orders_get") == 2 &&
                counter("positions_total") == 4 && counter("orders_total") == 4,
            "collection did not repeat complete unfiltered books for " + scenario);
    require(managed_open_capabilities_current(*result.snapshot, lease),
            "new snapshot is not current for " + scenario);
    require_read_only(journal);
    return *result.snapshot;
}

/// \brief Requires explicit fail-closed status with no snapshot or publication.
void reject(const std::string &scenario, Status expected, const AccountKey &account,
            const WindowsSingleWriterLease &lease, const WindowsFileJournalStore &journal) {
    configure(scenario);
    const auto result = collect_managed_open_capabilities(account, "EURUSD", lease);
    require(result.status == expected && !result.snapshot,
            "unexpected rejection for " + scenario + ": actual=" +
                std::to_string(static_cast<int>(result.status)) + " expected=" +
                std::to_string(static_cast<int>(expected)));
    require_read_only(journal);
}

/// \brief Checks supported observation modes and canonical source-derived quantity domains.
void test_domains(const AccountKey &account, const WindowsSingleWriterLease &lease,
                  const WindowsFileJournalStore &journal) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        const auto snapshot = collect("mode-" + std::to_string(mode), account, lease, journal);
        const auto &data = snapshot.data();
        require(data.symbol.trade_exemode == mode, "known execution mode was lost");
        require(data.normalization.scale == 2 && data.normalization.step_units == 1 &&
                    data.minimum_units == 1 && data.maximum_units == 10000 &&
                    data.directional_limit_units == 0,
                ".01 source normalization or zero limit was not preserved");
        require(!snapshot.blocks_unencumbered_open() && data.account_position_count == 0 &&
                    data.account_order_count == 0,
                "empty complete inventory was not retained");
        require(data.account.known_fields != 0 && data.symbol.known_fields != 0,
                "strictly known source fields were not marked");
    }
    const auto zero = collect("zero-filling", account, lease, journal);
    require(zero.data().symbol.filling_mode == 0 &&
                (zero.data().symbol.known_fields & MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE) != 0,
            "observed zero filling mask was confused with absence");
    const auto quarter = collect("quarter", account, lease, journal);
    require(quarter.data().normalization.scale == 2 &&
                quarter.data().normalization.step_units == 25 &&
                quarter.data().minimum_units == 25 && quarter.data().maximum_units == 10000 &&
                quarter.data().directional_limit_units == 20000,
            ".25 domain was not derived at its least scale");
    const auto integer = collect("integer", account, lease, journal);
    require(integer.data().normalization.scale == 0 &&
                integer.data().normalization.step_units == 1 &&
                integer.data().minimum_units == 1 && integer.data().maximum_units == 100,
            "integer domain was not derived at scale zero");
}

/// \brief Checks complete exposure, pending remainder and conservative OPEN blocking.
void test_books(const AccountKey &account, const WindowsSingleWriterLease &lease,
                const WindowsFileJournalStore &journal) {
    for (const auto *scenario : {"books", "permuted", "reduced-limit", "namedtuples"}) {
        const auto snapshot = collect(scenario, account, lease, journal);
        const auto &data = snapshot.data();
        require(data.buy_exposure_units == 19 && data.sell_exposure_units == 24 &&
                    data.account_position_count == 2 && data.account_order_count == 8 &&
                    data.symbol_position_count == 2 && data.symbol_order_count == 8 &&
                    snapshot.blocks_unencumbered_open(),
                "position/pending directional remainder or negative OPEN guard is wrong");
    }
    for (const auto *scenario : {"netting-exposure", "hedging-exposure"}) {
        const auto snapshot = collect(scenario, account, lease, journal);
        require(snapshot.blocks_unencumbered_open() && snapshot.data().sell_exposure_units == 3,
                "opposite/external position was treated as unencumbered OPEN");
    }
    const auto unrelated = collect("unrelated", account, lease, journal);
    require(!unrelated.blocks_unencumbered_open() &&
                unrelated.data().account_position_count == 1 &&
                unrelated.data().account_order_count == 1 &&
                unrelated.data().buy_exposure_units == 0 && unrelated.data().sell_exposure_units == 0,
            "unrelated-symbol rows altered requested-symbol exposure");
}

/// \brief Covers strict required fields, malformed source identities and volume rejection.
void test_rejections(const AccountKey &account, const WindowsSingleWriterLease &lease,
                     const WindowsFileJournalStore &journal) {
    for (const auto *field : {"server", "login", "margin_mode", "trade_mode",
                              "trade_allowed", "trade_expert"})
        reject("missing-account-" + std::string(field), Status::invalid_capabilities,
               account, lease, journal);
    for (const auto *field : {"connected", "trade_allowed", "tradeapi_disabled"})
        reject("missing-terminal-" + std::string(field), Status::invalid_capabilities,
               account, lease, journal);
    for (const auto *field : {"name", "trade_mode", "trade_exemode", "order_mode",
                              "filling_mode", "volume_min", "volume_max", "volume_step",
                              "volume_limit"})
        reject("missing-symbol-" + std::string(field), Status::invalid_capabilities,
               account, lease, journal);
    for (const auto *scenario : {
             "account-nul", "account-control", "account-truncated", "account-text-login",
             "account-bool-login", "account-unknown-margin", "account-unknown-trade",
             "account-int-permission", "terminal-int-permission", "symbol-wrong",
             "symbol-nul", "symbol-control", "symbol-truncated", "symbol-text-step",
             "symbol-bool-mode", "symbol-unknown-execution", "symbol-unknown-trade",
             "symbol-unknown-order", "symbol-unknown-filling", "symbol-nan", "symbol-inf",
             "symbol-zero-step", "symbol-negative-limit", "symbol-negative-zero-limit"})
        reject(scenario, Status::invalid_capabilities, account, lease, journal);
    for (const auto *scenario : {"account-wrong-login", "account-wrong-server", "account-switch",
                                 "late-account-switch"})
        reject(scenario, Status::account_mismatch, account, lease, journal);
    for (const auto *scenario : {"account-denied", "account-expert-denied",
                                 "terminal-disconnected", "terminal-denied",
                                 "terminal-api-disabled"})
        reject(scenario, Status::permission_denied, account, lease, journal);
    for (const auto *scenario : {"symbol-nextafter-step", "symbol-nextafter-min",
                                 "symbol-nongrid-max", "symbol-nongrid-limit",
                                 "symbol-tiny-step", "symbol-ambiguous-step",
                                 "row-nongrid", "row-nextafter", "overflow"})
        reject(scenario, Status::invalid_volume, account, lease, journal);
    for (const auto *scenario : {"positions-none", "orders-none", "positions-generator",
                                 "account-none", "total-none", "total-bool", "oversized-total",
                                 "count-mismatch", "duplicate-position", "duplicate-identifier",
                                 "duplicate-order", "row-zero-ticket", "row-bool-ticket",
                                 "row-unknown-position", "row-zero-identifier", "row-zero-volume",
                                 "row-nan", "row-inf", "order-close-by", "order-inactive",
                                 "order-missing-state", "malformed-unrelated"})
        reject(scenario, Status::incomplete_observation, account, lease, journal);
    for (const auto *scenario : {"count-change", "replace-position", "replace-order",
                                 "change-volume", "policy-change"})
        reject(scenario, Status::unstable_observation, account, lease, journal);
    reject("python-error", Status::python_error, account, lease, journal);
    reject("slow", Status::stale_observation, account, lease, journal);

    configure("empty");
    for (const auto &symbol : {std::string(), std::string("EURUSD\0alias", 12),
                               std::string(1024, 's'), std::string("\xff", 1)}) {
        const auto result = collect_managed_open_capabilities(account, symbol, lease);
        require(result.status == Status::invalid_request && !result.snapshot,
                "invalid requested identity was accepted");
    }
    require_read_only(journal);
}

/// \brief Runs lifecycle and concrete-lease freshness tests on the real runtime.
void run() {
    require(mt5bridge_abi_version() == MT5BRIDGE_ABI_VERSION, "ABI version mismatch");
    TemporaryDirectory directory;
    WindowsFileJournalStore journal(directory.path());
    require(journal.ready(), "cannot open empty durable journal");
    const AccountKey account{"Collector-Demo", 424242};
    Runtime runtime;
    std::optional<ManagedOpenCapabilitySnapshot> prior_owner;
    {
        WindowsSingleWriterLease lease(directory.path(), account);
        require(lease.ready(), "cannot acquire actual account lease");
        test_domains(account, lease, journal);
        test_books(account, lease, journal);
        test_rejections(account, lease, journal);

        const auto retained = collect("empty", account, lease, journal);
        const auto before = counter("positions_get");
        require(managed_open_capabilities_current(retained, lease) &&
                    counter("positions_get") == before,
                "local freshness check unexpectedly recollected broker data");
        std::this_thread::sleep_for(std::chrono::milliseconds{1100});
        require(!managed_open_capabilities_current(retained, lease),
                "retained observation survived its age bound");

        const auto old_runtime = collect("empty", account, lease, journal);
        require_read_only(journal);
        runtime.stop();
        require(!managed_open_capabilities_current(old_runtime, lease),
                "shutdown retained source authority");
        const auto stopped = collect_managed_open_capabilities(account, "EURUSD", lease);
        require(stopped.status == Status::runtime_unavailable && !stopped.snapshot,
                "collection without a running interpreter did not fail closed");
        runtime.start();
        require(!managed_open_capabilities_current(old_runtime, lease),
                "old runtime generation became current after restart");
        const auto restarted = collect("empty", account, lease, journal);
        require(restarted.runtime_generation() != old_runtime.runtime_generation(),
                "restart did not advance actual runtime generation");

        WindowsSingleWriterLease unavailable(directory.path(), account);
        require(!unavailable.ready(), "competing account lease unexpectedly acquired ownership");
        auto failed = collect_managed_open_capabilities(account, "EURUSD", unavailable);
        require(failed.status == Status::lease_lost && !failed.snapshot &&
                    !managed_open_capabilities_current(restarted, unavailable),
                "unavailable concrete lease authorized a snapshot");
        WindowsSingleWriterLease foreign(directory.path(), {"Collector-Demo", 424243});
        require(foreign.ready(), "foreign account lease could not be acquired");
        failed = collect_managed_open_capabilities(account, "EURUSD", foreign);
        require(failed.status == Status::lease_lost && !failed.snapshot &&
                    !managed_open_capabilities_current(restarted, foreign),
                "wrong account concrete lease authorized source facts");
        prior_owner = collect("empty", account, lease, journal);
    }
    WindowsSingleWriterLease successor(directory.path(), account);
    require(successor.ready(), "successor concrete lease could not acquire ownership");
    const auto fresh = collect("empty", account, successor, journal);
    require(prior_owner && fresh.fencing_token() > prior_owner->fencing_token() &&
                !managed_open_capabilities_current(*prior_owner, successor),
            "old account epoch survived concrete lease generation transfer");
    require_read_only(journal);
    runtime.stop();
}

} // namespace

/// \brief Runs always-on native collector checks against an owned Python runtime.
/// \return Zero only when every source, freshness and read-only regression passes.
int main() {
    try {
        run();
        std::cout << "runtime-owned managed OPEN capability collector checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

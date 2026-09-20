# mt5bridge_cpp

C++17 bridge embedding CPython to call the MetaTrader5 Python API from native apps (quotes, bars, orders). The host-facing surface is the Windows x64 `mt5_bridge.dll` with a small UTF-8 JSON C ABI.

## Quickstart

1. Clone the repository.
2. Build the bridge using MSVC or MinGW.
   - For a one-step MSVC build run `scripts\build_msvc.bat`.
   - Alternatively follow the manual CMake commands below.
3. Prepare the runtime environment (see [Runtime setup](#runtime-setup)).
4. Run `build\bin\Release\usage_example.exe` (Visual Studio) or the
   generator-specific `build\bin\usage_example.exe` to confirm the setup.

## Build

### MSVC

Run the helper script to configure and build a Release binary:

```bat
scripts\build_msvc.bat
```

To invoke CMake manually instead:

```powershell
# From a Developer PowerShell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Project Python environment

The live runtime and Windows smoke tests use the project-local Python 3.11
environment. Create it and install the pinned MetaTrader5/NumPy dependencies
with:

```powershell
.\setup_env.bat
```

Use `venv\Scripts\python.exe` for live checks. The embedded DLL must be built
against the same Python major/minor version as the installed `MetaTrader5`
wheel; configure CMake with `-DPython3_EXECUTABLE=...\venv\Scripts\python.exe`
when more than one Python installation is present.

### MinGW

The consumer headers and examples can be built with MinGW. The CPython-backed
runtime DLL is currently supported and tested only with MSVC because the
official Windows Python import library and extension wheels are MSVC-built.

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

## Runtime setup

1. Install MetaTrader 5 and log into an account.
2. Prepare an embeddable Python runtime. For example:

   ```bash
   python scripts/prepare_py_runtime.py --python-url https://www.python.org/ftp/python/3.11.5/python-3.11.5-embed-amd64.zip --wheels <numpy-wheel-url> <metatrader5-wheel-url> --output build/runtime
   ```

   The preparation script configures the embeddable interpreter's isolated
   `._pth` file and places third-party wheels under `build/runtime/Lib/site-packages`.
   Set `PYTHONHOME`/`PYTHONPATH` to that runtime when launching examples. The
   MetaTrader5 Python package discovers the logged-in terminal through its own
   `initialize()` call; this project does not read a `bridge.ini` file.
4. Run `build\bin\Release\usage_example.exe` (Visual Studio) or
   `build\bin\usage_example.exe` (single-config generators).

For a future distributable release, the supported one-file user experience is
planned as a self-extracting `mt5_bridge.dll` bootstrap. The bootstrap itself
will not link to CPython: it will verify and extract a bundled
`mt5_bridge_runtime.dll` plus the embeddable Python/NumPy/MetaTrader5 runtime
into a content-addressed per-user cache, then load the core during
initialization. The design and clean-machine acceptance checks are recorded in
[ADR-0002](docs/adr/0002-self-contained-runtime-dll.md); the current
development workflow still uses the explicit runtime directory.

## Example usage

```cpp
#include <mt5bridge.hpp>
#include <iostream>

int main() {
    mt5bridge::Client bridge;
    try {
        bridge.load();
        bridge.initialize();
        std::cout << bridge.eval(R"({"method":"terminal_info"})") << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The `mt5bridge::Client` header dynamically loads the DLL and does not require
Python, Jansson, or an import library. The plain C declarations are available
in `mt5bridge/abi.h`; high-volume tick/rate POD contracts are in
`mt5bridge/data.h`.

See the `examples` directory for runnable DLL-loading examples and
[`docs/architecture.md`](docs/architecture.md) for the layering and migration
plan. Development rules adapted from the PVS-Studio review of bloated
AI-generated C++ live in [`docs/development-rules.md`](docs/development-rules.md).
High-throughput ticks/rates use the typed POD data plane described in
[`docs/market-data-api.md`](docs/market-data-api.md); known MT5 recovery cases
are tracked in [`docs/mt5-quirks.md`](docs/mt5-quirks.md).
Trade submission is deliberately single-shot; the planned bounded,
ticket/identifier-graph reconciliation flow is documented in
[`docs/trade-api.md`](docs/trade-api.md) and
[`docs/adr/0004-trade-reconciliation.md`](docs/adr/0004-trade-reconciliation.md).
Managed lifecycle identities, close obligations, schedules, execution plans,
and the safety boundary around future side effects are documented in
[`docs/adr/0006-managed-trade-lifecycle.md`](docs/adr/0006-managed-trade-lifecycle.md).

## Typed trade observation quickstart

The Stage 1 observation surface is read-only: it observes account and symbol
capabilities, runs an advisory `order_check`, and copies active/history order,
position, and deal snapshots. It never calls `order_send`.

1. Build the `trade_observation_example` target with the runtime enabled:

   ```powershell
   cmake --build build --config Release --target trade_observation_example
   ```
2. Start and log in to the MetaTrader 5 terminal.
3. Make the Python package visible to the embedded runtime:

   ```powershell
   $env:PYTHONPATH = (Resolve-Path ..\..\..\venv\Lib\site-packages).Path
   ```

4. Run the example from the build output directory:

   ```powershell
   .\trade_observation_example.exe EURUSD
   ```

The program performs concrete observations:

- `Client::account_info()` prints the immutable server/login identity and
  permission fields. Check `known_fields` before using an optional capability;
  an absent `hedge_allowed` field is reported as unknown, never inferred from
  the margin mode.
- `Client::symbol_capabilities()` prints trade mode, execution mode, filling
  and order masks, volume limits, and the `CLOSEBY` capability. Its
  `known_fields` mask distinguishes a real zero from a missing Python field.
- `Client::order_check()` sends a market-buy-shaped request to the advisory
  MT5 validator. The example uses a recent ask and `volume_min` when both are
  available; otherwise it deliberately requests zero volume to demonstrate a
  safe rejection such as `Invalid volume`. Neither path has a trading side
  effect.
- `Client::orders()`, `positions()`, `history_orders()`, and `history_deals()`
  copy bounded typed snapshots and print their counts. Empty collections are
  valid; missing required graph fields fail closed.

Active queries use the same mutually-exclusive `symbol`/`group`/`ticket`
selectors as the documented MT5 overloads. History uses separate
`Mt5HistoryOrdersRequest` and `Mt5HistoryDealsRequest` types: the server is
queried by bounded time range and optional group, while order/deal/position
identity filters are applied locally to the returned evidence. The public
history window is inclusive at millisecond precision: the bridge widens the
Python query to a whole-second superset, then filters converted snapshots
locally (`time_done_msc` for history orders and `time_msc` for history deals).

For observation-only reconciliation, include `<mt5bridge.hpp>` and feed these
typed snapshots into `mt5bridge::ObservationGraph`. It scopes evidence by
`(server, login)`, keeps active/history namespaces separate, and exposes
deterministic provenance-preserving order/deal/position links. Mark complete
active domains explicitly; an observed empty active snapshot clears that
namespace, while history observations retain revision-tagged coverage windows.
Use the domain revision and baseline-aware history coverage helpers before
treating absence as evidence. The graph never calls MT5 or sends an order;
durable journal and managed `TradeId`/`OperationId` association remain the next
stage. Each graph has a process-local `instance_id()` and is non-copyable; a
`ReconciliationBaseline` captured from one graph cannot be reused with another
graph, even when account and revision values happen to match.

For pure observation reconciliation, capture a
`ReconciliationBaseline`, store it in the request's `std::optional` baseline,
apply fresh snapshots to the same graph, and evaluate explicit predicates with
`ReconciliationEngine`. This produces `PENDING`,
`CONFIRMED`, `NOT_OBSERVED`, `ACCOUNT_MISMATCH`, `TRADE_EVENT_GAP`, or
`AMBIGUOUS` without invoking `order_send`. The caller supplies
`deadline_expired` when its bounded wait has elapsed; an unsatisfied fresh
predicate remains `PENDING` before that point, and `NOT_OBSERVED` remains
unresolved for later reconciliation. The observation-only coordinator and
`DispatchConsistencyGate` add a synchronous provider seam and a pre-dispatch
freshness check. The coordinator rejects provider batches whose domains or
windows do not exactly match the request and verifies account identity before
and after collection. `ready` never submits an order, starts a worker, or
writes a journal; it is not an atomic cross-domain MT5 snapshot proof. Their
contract is recorded in
[`docs/adr/0009-observation-coordinator.md`](docs/adr/0009-observation-coordinator.md).

For the next observation-only step, collect accepted `ObservationSample` values
from `ObservationCoordinator` and feed two or more sequential samples into
`mt5bridge::EnvironmentConsistencyPolicy`. It requires one graph identity,
consecutive revisions, and the exact requested domains/windows to remain
account-scoped; it also checks visible order/deal/position links and requires
two consecutive coherent identity signatures before
returning `consistent`. Delayed publication such as a deal appearing before
its history order returns `awaiting_confirmation` only when both order
namespaces are requested; direct link conflicts, account changes, bounded
instability, and order links outside the requested history coverage return
explicit fail-closed states. Publication lag that persists through the bounded
observation budget becomes `unstable_environment`, not a false
`cross_view_mismatch`.
This policy still does not claim an atomic MT5 snapshot and does not call or
authorize `order_send`; its contract is recorded in
[`docs/adr/0010-environment-consistency.md`](docs/adr/0010-environment-consistency.md).

This Stage 2 slice adds `mt5bridge::OperationJournal` and
`mt5bridge::DispatchAdmissionBarrier`. The journal durably records the opaque
operation intent, AccountKey, managed IDs, and write-ahead states through a
caller-provided durable store. Windows consumers can use the Python-free
`mt5bridge::WindowsFileJournalStore` from the separate `mt5bridge::journal`
target; it uses bounded checksum-validated records and an atomic replace under
a directory lock. The barrier requires an opaque scope- and
revision-bound environment proof covering its configured domains/history
windows, the same current AccountKey and graph revision, no
unresolved operation or event gap, and a non-zero single-writer fencing token
before committing `dispatching`. The private one-shot backend consumes that
permit only after repeating the account/lease/revision checks, calls its
injected transport once, and persists the complete raw result before advancing
the lifecycle. `10018 MARKET_CLOSED` is recorded as deterministic
`rejected`; transport failures remain unresolved and are never retried. No
unmanaged public `order_send` is exposed.

The complete source is [`examples/trade_observation_example.cpp`](examples/trade_observation_example.cpp).

With a logged-in terminal, run the bounded native market-data smoke check from
the build output directory (optionally pass a broker-specific symbol):

```powershell
$env:PYTHONPATH = (Resolve-Path ..\..\..\venv\Lib\site-packages).Path
.\live_market_smoke.exe EURUSD
```

The check reads a bounded 72-hour tick window and seven days of M1 bars so it
also works when the terminal is started during a weekend, prints recovery
diagnostics, and shuts the bridge down before exiting. Set
`PYTHONPATH` to the project environment that contains `MetaTrader5` when the
embedded runtime cannot discover it automatically.

## Notes

- The current public contract is ABI 8. Tick POD records preserve separate
  integer `volume` and floating-point `volume_real` fields.
- The typed trade-observation surface exposes account/symbol snapshots,
  advisory `order_check`, and active/history order, position, and deal POD
  records through `trade.h`; it does not expose an unmanaged `order_send`.
- The legacy JSON `open_market_buy` convenience method is disabled in ABI 8;
  the Stage 2 journal now defines the pre-side-effect barrier, while the
  internal backend call remains a later slice.
- Realtime consumers use `mt5bridge_subscribe_ticks()` and host-driven
  `mt5bridge_process_events()`; overflow is reported as an explicit GAP event.
- Only 64‑bit Windows builds are supported.
- The CPython-backed runtime currently supports and is tested with Python 3.11.x.
- The C++ client resolves the DLL to an absolute path and restricts dependency
  lookup to the DLL directory and default safe Windows directories.
- The DLL must be shut down before `FreeLibrary`.
- Reinitialization after an owned CPython shutdown requires the documented
  live 100-cycle acceptance test; it is not yet a release guarantee.
- Issues and pull requests are welcome.

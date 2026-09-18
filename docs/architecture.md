# Architecture and modernization baseline

## Current shape

The project builds one Windows x64 shared library, `mt5_bridge.dll`. The DLL
embeds CPython, imports `MetaTrader5`, validates a small request object, calls a
named operation, and returns UTF-8 JSON or typed POD observations. C++
applications use the lightweight `mt5bridge::Client` facade (or the plain C
ABI); the Python binding uses ctypes.

The boundary is deliberately narrower than the implementation:

```text
consumer (C++ / ctypes)
        |  mt5bridge::Client or C ABI: UTF-8 JSON + POD snapshots
mt5_bridge.dll
        |  serialized lifecycle + GIL
request dispatcher
        |  Python objects
MetaTrader5 Python package -> terminal
```

The DLL does not expose STL, Jansson, Python objects, or third-party ownership.
This avoids allocator/CRT and ABI mismatches between a host and the DLL.
Because the implementation embeds CPython, a ctypes host must provide the same
Python major/minor runtime used to build the DLL (or use a packaged runtime).
The C ABI removes C++/JSON ABI coupling; it does not make CPython itself
version-independent.

## DLL-first contract

- `mt5bridge_initialize()` is idempotent and owns interpreter startup.
- `mt5bridge_eval_json()` accepts one JSON object and returns one allocated JSON
  string. The caller releases it with `mt5bridge_free()`.
- `mt5bridge_shutdown()` is idempotent, returns a status, and completes before
  unloading the DLL.
- ABI 8 is checked by both `mt5bridge::Client` and the ctypes adapter before
  use. POD sizes and field offsets are compile-time assertions in `data.h` and
  `trade.h`; the additive trade observation surface has its own API version.
- Steady-state calls that touch Python are serialized. Runtime-call admission
  checks the lifecycle state under `g_mutex`, then releases that state mutex
  before taking the interpreter mutex and calling Python. An in-flight
  admission counter keeps the interpreter alive until the call completes;
  shutdown closes admission and waits for that counter to reach zero before
  finalization. `initialize()` and `shutdown()` are explicit lifecycle
  exceptions because they transition interpreter ownership while holding the
  lifecycle and interpreter mutexes. Diagnostics are thread-local and are
  valid until the next call on the same thread.
- Native callbacks run without the Python GIL or runtime mutex held. Exported
  operations catch C++ exceptions before returning through the C ABI.
- The C++ client resolves the requested DLL to an absolute path and uses
  `LoadLibraryExW` with restricted dependency search directories.
- `mt5bridge::Client` enforces one initialized owner per host executable; the
  plain C ABI remains a process-global singleton and must use one lifecycle
  coordinator.
- New transports (named pipe, worker process, or another host) may be added
  behind the same request/response contract; they must not leak Python types.

## What we reuse from neighboring projects

- **vnote/vnote:** root routing `AGENTS.md` plus scoped child guides. This keeps
  high-value rules visible while avoiding a giant prompt for every source file.
- **log-it-cpp:** minimal diffs, explicit include contracts, focused build/test
  checks, and conventional commit discipline.
- **DataFeedHub:** domain-oriented directories, umbrella/public entry points,
  explicit feature boundaries, fixed-width DTOs, and concise Doxygen contracts.
- **optionx_cpp:** narrow modules, RAII, deterministic lifecycle, and reuse of
  shared event/task abstractions when asynchronous work is introduced. For this
  synchronous bridge, do not add an event bus merely to imitate that project.
- **qwen3-tts-bridge-cpp:** persistent runtime ownership, a narrow engine
  boundary, dynamic-library packaging concerns, and a staged path for a native
  backend. The selected native Qwen DLL remains a future adapter here; the MT5
  DLL must stay the stable host-facing boundary.

The consumer target is `mt5bridge::client` (INTERFACE); the heavy runtime target
is `mt5_bridge` and is optional via `MT5BRIDGE_BUILD_RUNTIME=OFF`. This keeps
`Python3::Python` a PRIVATE build dependency of the DLL rather than a dependency
of applications that only consume the client header.

## Public and private source layout

Source layout mirrors the component boundary:

```text
include/
├── mt5bridge.hpp
└── mt5bridge/
    ├── abi.h
    ├── client.hpp
    ├── data.h
    └── trade.h

src/
└── runtime/
    └── mt5_bridge.cpp
```

Everything below `include/mt5bridge*` is consumer-facing SDK/API. Everything
below `src/runtime/` is private implementation owned by the DLL and must not be
included by applications. When the runtime grows, private `.hpp` and `.cpp`
files should live side by side in `src/runtime/`; do not create a second
private include tree merely to mirror the public one. Keep
`src/runtime/mt5_bridge.cpp` as one implementation unit until a real
responsibility boundary justifies a split; directory shape alone is not a
reason to add speculative wrappers or adapters.

Bulk market-data contracts and MT5 recovery behavior are specified separately
in [market-data-api.md](market-data-api.md) and [mt5-quirks.md](mt5-quirks.md).
Trade identity, raw access, and reconciliation stages are specified in
[trade-api.md](trade-api.md) and [ADR-0004](adr/0004-trade-reconciliation.md).
The runtime-call admission and shutdown barrier are specified in
[ADR-0005](adr/0005-runtime-call-admission.md).
The managed trade identity, close-obligation, scheduling, and exit-policy
boundaries are specified in [ADR-0006](adr/0006-managed-trade-lifecycle.md).
The read-only account-scoped evidence graph is specified in
[ADR-0007](adr/0007-observation-graph.md) and exposed by
`include/mt5bridge/reconciliation.hpp`.
The observation-only predicate evaluator is specified in
[ADR-0008](adr/0008-observation-reconciliation.md) and exposed by
`include/mt5bridge/reconciliation_engine.hpp`.
The synchronous observation coordinator and pre-dispatch consistency gate are
specified in [ADR-0009](adr/0009-observation-coordinator.md) and exposed by
`include/mt5bridge/reconciliation_coordinator.hpp`.
The bridge never retries a side-effecting order implicitly.
The planned single-file runtime distribution is fixed in
[ADR-0002](adr/0002-self-contained-runtime-dll.md): a Python-free bootstrap DLL
embeds a verified payload, extracts it to a content-addressed per-user cache,
and loads `mt5_bridge_runtime.dll` during `mt5bridge_initialize()`. With the
realtime subscription ABI now defined, the next packaging milestone can
implement this without changing the data plane.

## Runtime lifecycle limit

Calling `mt5bridge_initialize()` again while the bridge is already initialized
is supported. The thread that creates an owned interpreter is its lifecycle
owner. `mt5bridge_shutdown()` returns a status and finalizes CPython only on
that thread; a cross-thread shutdown is rejected without touching Python and
reports an error through `mt5bridge_last_error()`. The C++ facade enforces the same rule and
keeps the DLL loaded until the owner thread performs shutdown; destroying an
initialized facade from another thread terminates the process rather than
silently poisoning global ownership.

Owned initialization uses CPython's `PyConfig` API, so `python_home` is copied
into CPython configuration rather than retained as a borrowed ABI pointer.

Repeated `initialize → query → shutdown → initialize` cycles after an owned
`Py_FinalizeEx()` are not yet a production guarantee: CPython extension modules
such as NumPy and MetaTrader5 may retain process-global state. Release
acceptance must run at least 100 cycles against the packaged Python runtime and
a live terminal. If that test is unstable, the supported contract becomes one
embedded-runtime lifetime per process, or the runtime moves to a restartable
worker process.

## Migration path

1. Keep the current embedded-Python backend as the release path.
2. Complete the typed raw observation surface in bounded slices: active orders,
   positions, history orders, and history deals. The ABI exposes these
   collection snapshots through matching C++ and ctypes facades. Keep every
   MT5 ticket,
   position identifier, order/deal link, reason, entry, volume, price, and
   external id needed by the future graph. No unmanaged public `order_send` is
   exposed.
3. Build the observation-only account-scoped graph and reconciliation
   evaluator.
   The current `ObservationGraph` stores deterministic ticket-keyed evidence
   and explicit order/deal/position links without side effects. Its global
   revision is supplemented by per-domain freshness and revision-tagged
   history coverage, so a later worker can prove post-baseline absence without
   treating stale evidence as current. A later journal layer may associate
   `TradeGroupId`, `TradeId`, `OperationId`, and `CloseObligation`; the graph
   must not infer them from raw snapshots. `ReconciliationEngine` currently
   evaluates explicit predicates over this evidence and leaves deadline
   ownership to its caller. `ObservationGraph` instances carry a process-local
   provenance identity and are non-copyable, so a baseline cannot silently move
   to another owner. `ReconciliationBaseline` is captured through the graph
   factory and exposes both that identity and revisions read-only. Requests carry
   the baseline as `std::optional`, making absence explicit rather than a
   default-invalid sentinel. A future worker/journal owner must retain the
   captured value and evaluate it against the same graph instance. Snapshot
   collection and durable operation state remain separate.
4. Add an observation coordinator and pre-dispatch consistency gate. The
   coordinator owns one graph loop, captures a baseline, collects authoritative
   account-wide snapshots through the typed client, applies them, and invokes
   the pure evaluator. The gate may return only evidence-based readiness or an
   explicit unresolved state; this slice still has no `order_send`, journal,
   worker thread, or side effect.
5. Add the durable dispatch journal and reconciliation barrier described in
   [trade-api.md](trade-api.md); commit `dispatching` before the one internal
   `order_send` and never resend after that barrier. Admission and the durable
   dispatch boundary must be designed together.
6. Add the high-level asynchronous `TradeManager` only after raw observations,
   journal recovery, and identity rules are covered by fake-runtime tests.
   Side-effecting methods must follow [ADR-0004](adr/0004-trade-reconciliation.md).
7. Add timed close obligations, then a separate execution planner for sliced
   entry/exit. Slicing must not be hidden inside a single-trade manager.
8. Add hybrid virtual/broker exit policies and a risk engine only after the
   lifecycle and reconciliation invariants are executable.
9. If startup/reliability requires process isolation, introduce a transport
   implementation behind the same C ABI; do not duplicate business methods.
10. Add a native or alternate MT5 backend only behind a backend interface after
   measuring lifecycle, error, and compatibility behavior.
11. After realtime ABI stabilization, ship an external runtime ZIP, split the
   bootstrap/core DLLs, and then produce the self-extracting artifact described
   by [ADR-0002](adr/0002-self-contained-runtime-dll.md).

## Sources

- VNote `AGENTS.md` (repository routing and scoped documentation pattern),
  consulted 2026-09-09.
- qwen3-tts-bridge-cpp `AGENTS.md` and `docs/cmp50hx-playback-investigation.md`,
  consulted 2026-09-09.
- DataFeedHub and optionx_cpp `AGENTS.md`, consulted 2026-09-09.

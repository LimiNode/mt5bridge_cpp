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

The consumer target is `mt5bridge::client` (INTERFACE); the Python-free
`mt5bridge::journal` target contains the concrete durable store, while the
heavy runtime target is `mt5_bridge` and is optional via
`MT5BRIDGE_BUILD_RUNTIME=OFF`. This keeps `Python3::Python` a PRIVATE build
dependency of the DLL rather than a dependency of applications that only
consume the client or journal target.

## Public and private source layout

Source layout mirrors the component boundary:

```text
include/
├── mt5bridge.hpp
└── mt5bridge/
    ├── abi.h
    ├── client.hpp
    ├── market.hpp
    ├── market/data.h
    ├── trade.hpp
    ├── trade/observation.h
    ├── observation.hpp
    ├── observation/{graph,coordinator,environment_consistency,worker}.hpp
    ├── reconciliation/{engine,operation_worker}.hpp
    └── dispatch/{journal,file_journal_store}.hpp

src/
└── runtime/
    └── mt5_bridge.cpp
```

The public dispatch domain is exposed through `dispatch.hpp` and its focused
headers under `dispatch/`. The latter is implemented by the Python-free
`src/runtime/file_journal_store.cpp` source in the separate
`mt5bridge::journal` target. The private one-shot execution seam lives in
`src/runtime/one_shot_backend.hpp/.cpp` and is built as
`mt5bridge::one_shot_backend`; the CPython-specific
`python_dispatch_transport.hpp/.cpp` adapter is compiled only into the DLL.
Neither private seam is part of the consumer SDK.

Everything below `include/mt5bridge*` is consumer-facing SDK/API. The source
under `src/runtime/` is implementation owned by the native targets and must
not be included by applications. When the runtime grows, private `.hpp` and
`.cpp` files should live side by side in `src/runtime/`; do not create a
second private include tree merely to mirror the public one. Keep
`src/runtime/mt5_bridge.cpp` as one implementation unit until a real
responsibility boundary justifies a split; directory shape alone is not a
reason to add speculative wrappers or adapters.

Bulk market-data contracts and MT5 recovery behavior are specified separately
in [market-data-api.md](market-data-api.md) and [mt5-quirks.md](mt5-quirks.md).
The rate data-plane extension is immutable V1 (`Mt5RateCoverageV1` and
`mt5bridge_rate_buffer_coverage_v1`); strict C++ callers must opt into
`Client::query_rates_range()` when they need explicitly best-effort rows.
Trade identity, raw access, and reconciliation stages are specified in
[trade-api.md](trade-api.md) and [ADR-0004](adr/0004-trade-reconciliation.md).
The runtime-call admission and shutdown barrier are specified in
[ADR-0005](adr/0005-runtime-call-admission.md).
The private execution-lane scheduler is specified in
[ADR-0018](adr/0018-execution-lane-fairness.md): trade-critical calls receive
bounded priority over queued bulk market-data work without changing the public
ABI or preempting an in-flight Python operation.
The production account ownership primitive is specified in
[ADR-0019](adr/0019-production-account-lease.md): a Windows-exclusive
account lock and durable monotonic fencing epoch implement the existing
`SingleWriterLease` seam without adding a side-effecting public API.
The immutable post-dispatch evidence contract is specified in
[ADR-0020](adr/0020-durable-reconciliation-descriptor.md): dispatch admission
persists the reconciliation baseline and predicates before opening the
non-resendable barrier, and recovery rejects records that lack them.
The managed trade identity, close-obligation, scheduling, and exit-policy
boundaries are specified in [ADR-0006](adr/0006-managed-trade-lifecycle.md).
The read-only account-scoped evidence graph is specified in
[ADR-0007](adr/0007-observation-graph.md) and exposed by
`include/mt5bridge/observation/graph.hpp` (with the old path retained as a
forwarding header).
The observation-only predicate evaluator is specified in
[ADR-0008](adr/0008-observation-reconciliation.md) and exposed by
`include/mt5bridge/reconciliation/engine.hpp` (with the old path retained as a
forwarding header).
The synchronous observation coordinator and pre-dispatch consistency gate are
specified in [ADR-0009](adr/0009-observation-coordinator.md) and exposed by
`include/mt5bridge/observation/coordinator.hpp`.
The bounded cross-view environment policy is specified in
[ADR-0010](adr/0010-environment-consistency.md) and exposed by
`include/mt5bridge/observation/environment_consistency.hpp`. It checks sequential
observation batches for account continuity, direct identity-link conflicts,
and a repeated stable evidence signature; it does not claim that MT5 supplied
an atomic snapshot.
Progressive deep tick synchronization is specified in
[ADR-0015](adr/0015-progressive-tick-history-bootstrap.md); it is bounded and
does not alter the fixed C data-plane ABI. The caller-driven reconciliation
worker is specified in [ADR-0016](adr/0016-caller-driven-reconciliation-worker.md)
and keeps operation recovery outside the runtime thread and side-effect API.
The durable operation state machine and pre-side-effect barrier are specified
in [ADR-0011](adr/0011-durable-dispatch-admission.md), while the concrete
Windows file-backed store is specified in
[ADR-0012](adr/0012-windows-file-journal-store.md). The store is a separate
Python-free native target and never adds an unmanaged `order_send` surface.
The guarded one-shot execution seam and broker rejection handling are specified
in [ADR-0013](adr/0013-one-shot-backend.md); its terminal adapter remains
private and must preserve the same account and fencing checks. The embedded
Python implementation of that seam is specified in
[ADR-0014](adr/0014-private-python-dispatch-transport.md).
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

## Maintenance backlog

- **Windows handle RAII cleanup:** add one private `UniqueHandle` wrapper for
  `INVALID_HANDLE_VALUE`-based `HANDLE` ownership, then use it in
  `WindowsFileJournalStore` lock, read, and write paths. Preserve the current
  status-bearing errors and atomic replacement behavior; add focused failure
  tests for open/read/write cleanup. This is an implementation cleanup only:
  it must not change the C ABI, journal format, or recovery semantics.

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
5. Add the bounded environment-consistency policy. Feed it two or more
   coordinator-created samples with one graph identity and consecutive
   revisions before a future dispatch layer: a deal that becomes visible
   before its history order is `awaiting_confirmation`, direct link conflicts
   are `cross_view_mismatch`, and changing coherent samples exhaust as
   `unstable_environment`. This remains observation-only and has no journal,
   worker, or `order_send` side effect.
6. Add the durable dispatch journal and admission barrier described in
   [ADR-0011](adr/0011-durable-dispatch-admission.md). The current slice
    persists opaque intent payloads, separates journal state from
    `OperationState`, verifies AccountKey/environment/fencing evidence, and
    commits `dispatching` before returning a non-resendable permit. It still
    performs no backend call and exposes no `order_send`. The concrete
    `WindowsFileJournalStore` from [ADR-0012](adr/0012-windows-file-journal-store.md)
    supplies bounded, checksum-validated, atomically replaced records for this
    seam, with status-bearing single-record recovery and complete restart
    enumeration.
7. Add the internal one-shot backend that consumes that permit and keeps the
   same writer ownership through the dispatch barrier and call. The backend
   re-reads the live account through a `CurrentAccountProbe` after durable
   `submitting` and immediately before transport, then persists the full raw
   result before classifying deterministic broker rejections such as `10018
   MARKET_CLOSED`; it never infers session state from `trade_mode` or
   `order_check` and never retries transport failures. Bind that seam to the
   private embedded-Python `MetaTrader5.order_send` adapter and cover its
   complete-result, exception, malformed-result, and account-mismatch paths
   with a fake runtime. A successful broker result is only a reconciliation
   seed, not proof of final fill.
8. Add startup recovery orchestration and the reconciliation worker around the
   durable journal and observation graph. Only after these owner-loop and
   identity rules are covered by fake-runtime tests should the high-level
   asynchronous `TradeManager` be introduced. Side-effecting methods must
   follow [ADR-0004](adr/0004-trade-reconciliation.md).
   The owner-loop policy is specified in
   [ADR-0017](adr/0017-operation-recovery-and-reconciliation-worker.md): every
   recovered post-dispatch record is observation-only and never resendable.
9. Add timed close obligations, then a separate execution planner for sliced
   entry/exit. Slicing must not be hidden inside a single-trade manager.
10. Add hybrid virtual/broker exit policies and a risk engine only after the
   lifecycle and reconciliation invariants are executable.
11. If startup/reliability requires process isolation, introduce a transport
   implementation behind the same C ABI; do not duplicate business methods.
12. Add a native or alternate MT5 backend only behind a backend interface after
   measuring lifecycle, error, and compatibility behavior.
13. After realtime ABI stabilization, ship an external runtime ZIP, split the
   bootstrap/core DLLs, and then produce the self-extracting artifact described
   by [ADR-0002](adr/0002-self-contained-runtime-dll.md).

## Sources

- VNote `AGENTS.md` (repository routing and scoped documentation pattern),
  consulted 2026-09-09.
- qwen3-tts-bridge-cpp `AGENTS.md` and `docs/cmp50hx-playback-investigation.md`,
  consulted 2026-09-09.
- DataFeedHub and optionx_cpp `AGENTS.md`, consulted 2026-09-09.

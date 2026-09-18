# Trade API contract

This document defines the trade layer planned above the existing MT5 Python
bridge. It records what can be proven from the terminal and what must remain
explicitly uncertain.

## Implementation stages

1. **Raw typed observation:** `order_check`, active orders, positions, history
   orders, history deals, and symbol/account capabilities. This stage has no
   public side-effecting send method.
2. **Observation graph and reconciliation:** key immutable MT5 evidence by
   account and ticket/identifier, then reconcile bounded snapshots without
   sending side effects.
3. **Durable dispatch and reconciliation:** persist the operation journal,
   execute one `order_send` behind the durable dispatch barrier, and use
   ticket-keyed snapshots and bounded overlap reads to build a graph of
   independently observed entities.
4. **TradeManager:** stable logical IDs, asynchronous worker delivery, and
   callbacks/state events.
5. **Managed policies:** timed close obligations, execution planning, hybrid
   exits, and risk guards are layered above the reconciled manager.
6. **Optional native backend:** an MQL5 EA/service provides
   `OrderSendAsync`/`OnTradeTransaction` records through IPC when the Python
   backend's synchronous call is insufficient.

The stages are deliberately ordered. A convenience `open_async()` built on a
single JSON `order_send` result would hide the hard cases instead of solving
them.

The first stage is intentionally “raw”: it must not synthesize a managed trade
from one result object. It defines the typed request/result structures and
capability records needed to explain why a broker accepted, delayed, partially
filled, or rejected an operation. Stage 1 does not expose an unmanaged
side-effecting `order_send` to production callers; that primitive remains
internal until stage 2 supplies durable intent, account/lease verification,
and the non-resendable dispatch barrier.

## Implemented Stage 1 slice

The current ABI 8 implementation exposes the first bounded observation slice
in [`include/mt5bridge/trade.h`](../include/mt5bridge/trade.h):

- `mt5bridge_account_info()` returns account identity, permissions, margin mode,
  and balance/equity fields as a fixed-size `Mt5AccountInfo` snapshot. Its
  `known_fields` mask makes an unavailable permission (for example
  `hedge_allowed`, which current Python packages do not expose) explicit.
- `mt5bridge_symbol_capabilities()` returns execution, filling, expiration,
  sizing, stop/freeze, and `SYMBOL_ORDER_CLOSEBY` capability bits. The
  `trade_exemode` field is distinct from `trade_mode`; use the symbol
  `known_fields` mask before resolving a filling policy.
- `mt5bridge_order_check()` forwards a plain-C request to the advisory MT5
  `order_check()` call and returns the `MqlTradeCheckResult` fields
  (retcode/comment and projected margin values). A rejected check is still a
  successful transport call; it never submits an order. Every documented
  result field is required; a truncated backend record fails closed instead of
  becoming a result containing ambiguous zero/default values.
  `retcode_external` is deliberately reserved for the future `order_send`
  result.

The collection surface is also typed and read-only:

- `mt5bridge_query_orders()` and `mt5bridge_query_positions()` return bounded
  active snapshots.
- `mt5bridge_query_history_orders()` and `mt5bridge_query_history_deals()`
  return snapshots for an inclusive UTC millisecond window.
- `Client::orders()`, `positions()`, `history_orders()`, and `history_deals()`
  and the ctypes adapter copy the records into caller-owned storage.

Each collection buffer is released with its matching `*_buffer_free()` export;
an empty sequence is a successful observation. `None` is always a failed MT5
query, even when `last_error()` happens to report success; it is never treated
as an empty snapshot. Required graph fields missing from a namedtuple or mapping are an ABI
error. Optional fields are represented by `known_fields`, so zero is never an
absent-value sentinel. Seconds-only MT5 timestamps are converted to signed
Unix milliseconds with range checks.

The active [`orders_get()`](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersget_py)
and [`positions_get()`](https://www.mql5.com/en/docs/python_metatrader5/mt5positionsget_py)
selectors follow the documented MT5 overloads: at most one of `symbol`,
`group`, or `ticket` is sent to the server. `POSITION_IDENTIFIER` is a local
post-filter because it is not a `positions_get()` selector. History requests
always query a bounded `from/to` window with an optional server-side `group`;
order/deal ticket and position filters are applied locally to the returned
evidence. History deals distinguish `DEAL_TICKET`, `DEAL_ORDER`, and
`DEAL_POSITION_ID` instead of overloading one ambiguous `ticket` field; see
the [`history_deals_get()`](https://www.mql5.com/en/docs/python_metatrader5/mt5historydealsget_py)
overloads.

The public history window is inclusive and millisecond-precise. Because the
MetaTrader history selectors are defined in whole seconds, the bridge expands
the Python query to a second-aligned superset (`floor(from_msc)` through
`ceil(to_msc)`) and then applies the exact millisecond filter after converting
the records to POD snapshots. History orders are filtered by `time_done_msc`
(their completion/execution time), not by `time_setup_msc`; history deals are
filtered by `time_msc`. This prevents records from an adjacent millisecond
window from becoming reconciliation evidence.

The snapshots retain the evidence needed by reconciliation: orders keep
`ticket`, `position_id`, `position_by_id`, state/reason, volumes, prices,
setup/done/expiration timestamps, symbol/comment, and external ID; positions
keep ticket/`POSITION_IDENTIFIER`, type/reason, volume, prices, profit/swap,
update timestamps, symbol/comment, and external ID; deals keep ticket,
`DEAL_ORDER`, `DEAL_POSITION_ID`, entry/type/reason, volume/price,
profit/commission/swap/fee, time, symbol/comment, and external ID. Consumers
must key history by identifiers rather than array order or the last response
element.

The C++ facade exposes these operations as `Client::account_info()`,
`Client::symbol_capabilities()`, and `Client::order_check()`. Namedtuple and
mapping results are converted under runtime admission and the GIL into POD
values; no Python object crosses the ABI. Each snapshot preserves
all graph-relevant MT5 evidence instead of collapsing a response to one
“latest” ticket. A public side-effecting `order_send` remains intentionally
absent until the durable journal and reconciliation barrier are implemented.

The managed lifecycle boundaries are recorded in
[ADR-0006](adr/0006-managed-trade-lifecycle.md). `TradeGroupId`, `TradeId`,
`OperationId`, and `CloseObligation` are domain identities above raw MT5
evidence; they must never be inferred from one observation response.

## Observation-only graph

The C++ header [`reconciliation.hpp`](../include/mt5bridge/reconciliation.hpp)
provides `mt5bridge::ObservationGraph` for the next stage. It accepts batches
of the typed snapshots together with an immutable `(server, login)`
`AccountKey` and explicit `ObservationDomain` bits. `login == 0` is rejected,
and `make_account_key()` requires the account server/login `known_fields` bits.
Active orders and positions are authoritative full snapshots when their domain
is marked observed; an empty vector therefore clears that namespace, while an
omitted domain leaves it unchanged. History orders and deals are positive
ticket-keyed evidence. Non-empty filtered history evidence may omit coverage;
an empty history result must include a valid window, and supplied windows are
retained as revision-tagged inclusive coverage only for account-wide unfiltered
reads. Coverage entries from different revisions are never merged without
preserving their provenance; `history_*_coverage()` returns
`ObservationCoverage` entries rather than naked windows.

Graph identity fields are also fail-closed: orders require known ticket and
`ORDER_POSITION_ID`, positions require known ticket and
`POSITION_IDENTIFIER`, and deals require known ticket, `DEAL_ORDER`, and
`DEAL_POSITION_ID`. When a history coverage window is supplied, the native
history time field must be marked known as well. Known zero values remain
usable; an unset known bit is never replaced by a default zero.

The graph exposes deterministic, provenance-preserving links for
`ORDER_POSITION_ID`, `DEAL_ORDER`, `DEAL_POSITION_ID`, and
`POSITION_IDENTIFIER`; active-order and history-order links are separate API
methods.

The graph is intentionally observation-only: it does not call MetaTrader,
generate managed `TradeId`/`OperationId` values, persist a journal, or invoke
`order_send`. An unbound graph adopts the first valid account; a different
account is rejected atomically, so account-switch evidence can never be mixed
into the existing graph. Invalid primary tickets and missing graph-identity
`known_fields` fail closed. Every accepted batch advances a monotonic global
revision and updates only the observed domains' revisions. Use
`domain_revision()` for active/position freshness, and use the revision-tagged
`history_*_coverage()` or `history_*_covered(window, since_revision)` for
baseline-aware history absence proofs. Positive history evidence without a
coverage window does not prove absence. History tickets expose their own last
evidence revision, so an old retained ticket cannot satisfy a post-baseline
presence predicate. `clear_evidence()` retains the account scope while
removing records, coverage, and freshness metadata. Every graph has a
process-local `instance_id()`; `ObservationGraph` is non-copyable and
non-movable, so this provenance cannot be confused with another graph that
happens to use the same account and revision numbers. The graph is single-owner
state; callers must serialize access.

## Observation-only reconciliation predicates

The header [`reconciliation_engine.hpp`](../include/mt5bridge/reconciliation_engine.hpp)
provides `ReconciliationEngine` for evaluating evidence without side effects.
Capture a `ReconciliationBaseline` before the operation, collect fresh
authoritative observations into `ObservationGraph`, and evaluate explicit
predicates such as `require_active_order()`, `require_position_absent()`, or
`require_history_deal_absent()`.

Create the baseline only with `capture_reconciliation_baseline()`. The returned
graph provenance and revision fields are read-only; `ReconciliationRequest`
stores the baseline as `std::optional`, so a missing capture is explicit and
cannot be confused with a default-invalid value. This keeps application code
from editing one domain counter or reusing a baseline from another graph. A
baseline from a different process-local graph produces `AMBIGUOUS` with
`ReconciliationReason::graph_mismatch`. The future worker/journal owner should
capture and retain this value itself.

Active and position predicates require a domain revision newer than the
baseline. History presence predicates require a per-ticket evidence revision
newer than the baseline history revision; an old ticket retained in the
positive-evidence map is not reused. Any bounded history predicate also
requires the corresponding native millisecond time bit; missing time is
reported as contradictory evidence rather than matched using a default value.
History absence predicates require a complete revision-filtered coverage
window. Malformed predicates and baselines with impossible revision ordering
are rejected before graph state is evaluated.

The evaluator returns `PENDING`, `CONFIRMED`, `NOT_OBSERVED`,
`ACCOUNT_MISMATCH`, `TRADE_EVENT_GAP`, or `AMBIGUOUS`. It does not call the
runtime, write a journal, or invoke `order_send`. `TRADE_EVENT_GAP` is supplied
explicitly by a caller whose hint/event stream was incomplete; it is not
invented from a snapshot alone. A fresh mismatch remains `PENDING` until the
caller sets `ReconciliationRequest::deadline_expired`; only then can it become
`NOT_OBSERVED`. That outcome is unresolved and must not stop later
reconciliation. Use `ReconciliationResult::resolved()` only for
`CONFIRMED`, `ACCOUNT_MISMATCH`, and `AMBIGUOUS`. A future
`ReconciliationWorker` may own the snapshot collection loop, but must be the
component that builds authoritative account-wide batches. The result counters
separate stale/missing observations from contradictory evidence, so callers
can continue polling while `PENDING` without interpreting a missing predicate
as a final outcome.

## Quickstart scenarios

The runnable [`trade_observation_example.cpp`](../examples/trade_observation_example.cpp)
shows the intended caller sequence:

1. Build the example with `cmake --build build --config Release --target trade_observation_example`,
   then load `mt5_bridge.dll` with `mt5bridge::Client` and initialize the terminal.
2. Read `Client::account_info()` and retain the server/login `AccountKey`
   evidence. Test `known_fields` before making decisions from optional
   permissions; unknown is not the same as `false`.
3. Read `Client::symbol_capabilities("EURUSD")`. Use `trade_mode`,
   `trade_exemode`, filling/order masks, and volume/tick constraints to choose
   a candidate request policy. Test the corresponding symbol known bits first.
4. Build `Mt5OrderCheckRequest` and call `Client::order_check()`. The returned
   `retcode` and `comment` explain the advisory result; even a rejection is a
   successful transport call and no order is sent.
5. Shut down the client before it is destroyed or the DLL is unloaded.

This sequence is useful for two common scenarios:

- **Pre-trade validation:** inspect account/symbol capabilities, then run
  `order_check` to explain an invalid volume, filling mode, or permission.
- **Startup diagnostics:** record the account identity and known-field masks,
  then fail closed if a required capability is unavailable. The later Stage 2
  journal can safely reuse these observations without guessing missing values.

The example uses the standard MT5 numeric values `TRADE_ACTION_DEAL = 1` and
`ORDER_TYPE_BUY = 0` only to shape the advisory request. When a recent ask and
known volume limits are available, it checks `volume_min`; otherwise it uses
zero volume to produce a safe diagnostic rejection. It intentionally does not
expose or invoke `order_send`; durable dispatch is a separate stage.

## Identity model

The managed runtime owns three logical levels:

```text
TradeGroupId / ExecutionPlanId
  ├── TradeId
  │   ├── OperationId OPEN
  │   ├── OperationId CLOSE ...
  │   └── CloseObligation
  └── TradeId ...
```

- `TradeGroupId` identifies one signal or desired exposure plan.
- `TradeId` identifies one independently managed logical allocation. On a
  netting account it may be a virtual allocation rather than a separate
  terminal position.
- `OperationId` identifies one concrete side-effect attempt and is never reused
  for a blind retry.
- `CloseObligation` is the durable desired end state, not another attempt.

The bridge's raw identity evidence remains separate:

- `TradeId` — one logical lifecycle, possibly including open, partial fills,
  modifications, and close.
- `OperationId` — one side effect such as open, cancel, modify, or close.

MT5 identifiers are stored as separate optional evidence:

```text
TradeId
  ├── OperationId[]
  │     └── request_id?       (store whenever MT5 returns it)
  ├── order_ticket[]
  ├── deal_ticket[]
  ├── position_identifier[]
  └── position_ticket[]
```

`POSITION_IDENTIFIER` links order/deal history to a position; it is not the
same field as `POSITION_TICKET`. Neither is the bridge's `TradeId`.
`request_id` is available in the Python `MqlTradeResult` too, but without
`OnTradeTransaction` it is diagnostic/correlation evidence only. A native MQL
backend can additionally correlate the request transaction with it. The bridge
stores it with an explicit `TerminalSessionId`; it must never be correlated
across terminal sessions or restarts.

### Close obligations and managed policies

A close obligation is satisfied only after reconciliation proves that the
target exposure is gone. A timeout or connection error creates an ambiguous
attempt and forbids a blind resend; a later attempt may target only the
currently reconciled remainder with a new `OperationId`. `DONE_PARTIAL`,
`POSITION_CLOSED`, and `INVALID_CLOSE_VOLUME` therefore require a fresh
snapshot before the next decision.

Timed trades persist their schedule in the journal:

```text
CloseSchedule::none()
CloseSchedule::after(duration)  # anchored to the first confirmed fill
CloseSchedule::at(absolute_utc)
```

After restart, an expired schedule resumes or creates its `CloseObligation`.
Sliced entry/exit belongs to a separate `ExecutionPlan`/`ExecutionPlanner`,
which may stop creating new `TradeId` values while continuing to manage those
already created. Virtual strategy exits should be paired with a wider broker
disaster stop when the risk policy requires protection from process loss.

## State and certainty

The lifecycle state is not a binary success flag:

```text
OperationState:
queued -> prechecking -> submitting -> accepted -> reconciling
                                      ├── partially_filled -> filled
                                      ├── rejected/failed
                                      ├── cancelled/expired
                                      └── ambiguous

TradeState:
pending -> partially_open -> open -> reducing -> closing -> closed
                                  └───────────────> reversed
```

The canonical `OperationState` vocabulary is `queued`, `prechecking`,
`submitting`, `accepted`, `reconciling`, `partially_filled`, `filled`,
`cancelled`, `expired`, `rejected`, `failed`, and `ambiguous`. `cancelled` and
`expired` are distinct terminal causes.

Certainty is reported independently:

```text
provisional -> server_confirmed -> reconciled
                         \-> ambiguous
```

For example, `retcode == DONE` with `deal == 0` can be
`accepted/server_confirmed`, but it is not `open/reconciled` until a deal and
the relevant position evidence are observed.

Reconciliation outcomes and reasons are metadata alongside these three
dimensions; they are not a fourth lifecycle enum. `PENDING` means observation
is still in progress, `CONFIRMED` means matching server evidence was found,
`NOT_OBSERVED` means the deadline passed without evidence, and
`ACCOUNT_MISMATCH` means observation stopped because the terminal account
changed, and `AMBIGUOUS` means no unique attribution could be proven.
`TRADE_EVENT_GAP` (or another overflow reason) means a hint stream was
incomplete and an authoritative snapshot/query is required. These outcomes
may accompany `OperationState::reconciling` with provisional certainty. An
unresolved `NOT_OBSERVED` operation can later resolve as
`ReconciliationOutcome::CONFIRMED` with `Certainty::reconciled`, or as
`ReconciliationOutcome::AMBIGUOUS` with `Certainty::ambiguous`.

The canonical outcome vocabulary is:

```text
ReconciliationOutcome:
PENDING, CONFIRMED, NOT_OBSERVED, ACCOUNT_MISMATCH, TRADE_EVENT_GAP, AMBIGUOUS
```

## Submission and reconciliation algorithm

```text
created
  -> prechecked
  -> dispatch_intent_persisted
  -> verify AccountKey / single-writer lease
  -> dispatching DURABLY COMMITTED
  -> order_send EXACTLY ONCE
  -> result_persisted
  -> reconciling
```

The states above are reached after generating `TradeId` + `OperationId`,
capturing a before-snapshot, resolving symbol execution/filling capabilities,
and running `order_check`. The durable `dispatching` record is committed
before entering `order_send`, not after it. Only then are the raw result,
`retcode_external`, and every non-zero ID persisted, followed by bounded
snapshots of orders, deals, positions, and history orders.

If `order_send` returns a timeout, the bridge performs no second send. It
reconciles the before/after observations and returns `PENDING`, `CONFIRMED`,
`NOT_OBSERVED`, or `AMBIGUOUS` with diagnostics. `NOT_OBSERVED` means only
that no evidence was visible before the deadline; it does not prove the server
did not execute the request and never permits a retry. Two concurrent
identical requests with no correlation evidence must remain `AMBIGUOUS`;
matching by symbol/volume/time, comment, or magic cannot safely choose one.

The initial reconciliation deadline is a wait limit, not the end of the
operation lifetime. After `NOT_OBSERVED`, the journal operation remains
unresolved and later snapshots, reconnects, history refreshes, or a restart may
emit a new transition to `CONFIRMED`, `RECONCILED`, or `AMBIGUOUS`.

Before the side effect, the operation journal durably records its
`AccountKey` (at minimum server and login), `TradeId`, `OperationId`, request
payload, and `dispatch_intent_persisted`. This record is still pre-side-effect:
recovery may reacquire the same lease, re-check the account, advance to
`dispatching`, and perform the one send. The `dispatching` record is a durable
may-have-been-sent barrier. On restart, a `dispatching` record with no result
is reconciled from snapshots and is never resent; this includes a crash after
the barrier but before entering `order_send()`. The system therefore provides
at-most-once dispatch, not exactly-once delivery. After the call it records
the raw result before moving to `reconciling`.

`AccountKey` is immutable for the operation and scopes graph keys as
`(AccountKey, OrderTicket)`, `(AccountKey, DealTicket)`, and
`(AccountKey, PositionIdentifier)`. It contains the terminal `server` and
`login`; `ACCOUNT_TRADE_ALLOWED`, `ACCOUNT_TRADE_EXPERT`, and other trade
permissions are capabilities checked separately. Immediately before any side
effect the current terminal account must match the immutable key, the required
capabilities must allow the request, and the process must hold the matching
single-writer lease. A mismatch prevents sending or advancing to `dispatching`.
If the account changes during reconciliation, suspend observation with
`ACCOUNT_MISMATCH`; never attach the new account's records to the old graph. A
managed TradeManager uses a single-writer lease per AccountKey. The lease must
be an OS-backed exclusive ownership primitive held continuously through the
dispatch barrier and `order_send`, or a fencing-token protocol; a bare
time-based lease without fencing is insufficient because an expired owner
could continue sending after a new owner takes over.

History reads use sets/maps keyed by tickets and bounded overlap snapshots.
They never rely on chronological order, the last array element, or an
append-only assumption. A deal may be visible before its history order, and a
history order may appear in a different position after a refresh. The graph
retains at least `DEAL_ORDER`, `DEAL_POSITION_ID`, `DEAL_ENTRY`,
`DEAL_REASON`, `ORDER_POSITION_ID`, `ORDER_POSITION_BY_ID`, `ORDER_REASON`,
position ticket/identifier/reason, and exchange-provided external IDs. The
`DEAL_ENTRY_INOUT` and `DEAL_ENTRY_OUT_BY` edges are required for netting
reversals and CloseBy, respectively.

## Account and execution semantics

- **Hedging:** multiple independent positions may correspond to one logical
  trade or operation.
- **Netting:** several logical trades can contribute to one symbol position.
  Closing one `TradeId` therefore uses an attribution ledger. The policy is
  explicit virtual lots (FIFO, LIFO, or pro-rata may be selected by the
  manager); an unconfigured netting close or an external mutation that makes
  remaining volume unknowable is `AMBIGUOUS`.
- **Partial fills:** one logical operation can produce multiple deals and a
  pending remainder. Do not replace the deal list with one “final ticket”.
- **Pending/limit orders:** retain the order while fills arrive; cancellation
  preserves already filled volume.
- **Execution/filling modes:** inspect symbol capabilities and resolve a
  compatible `FOK`, `IOC`, `RETURN`, or `BOC` policy before `order_check`/send.
  Also validate expiration/time-in-force, GTC mode, stops/freeze levels,
  volume min/max/step/limit, tick size, and `SYMBOL_ORDER_CLOSEBY`. Account
  capabilities include margin mode, FIFO-close, hedge permission,
  `ACCOUNT_TRADE_ALLOWED`, `ACCOUNT_TRADE_EXPERT`, and trade permission.
- **External changes:** manual trades, SL/TP, expiration, and broker/exchange
  execution differences are recorded as graph observations, not silently
  attributed to a bridge operation.

## Backend boundary

The current Python package has synchronous `order_check`, `order_send`,
`history_orders_get`, `history_deals_get`, `orders_get`, and `positions_get`.
The package's `order_send` is an implementation primitive, not a public raw
bridge method until stage 2 supplies the journal barrier. The package also
does not expose terminal `OrderSendAsync` or `OnTradeTransaction`. The first
production manager can therefore be asynchronous to the C++ caller by running
these calls on a dedicated worker and delivering bounded events, but it is not
terminal-native async.

`order_check` is advisory: a successful check does not reserve price,
liquidity, or permissions for the subsequent send. Return codes are
operation-aware: `PLACED` is a valid pending-order result, `DONE_PARTIAL` is
not by itself a terminal operation. `DONE_PARTIAL` proves partial execution,
but terminality depends on operation kind, filling policy, execution mode, and
the reconciled active-order remainder: `IOC` may cancel the remainder while
`RETURN` may keep it active. `TIMEOUT`, `REQUOTE`, and `REJECT` remain distinct.

An optional MQL5 backend may later forward only copied transaction records into
an IPC ring. Its `request_id` is valid for the request transaction, not as a
universal ID for every later `ORDER_*`, `DEAL_*`, `HISTORY_*`, or `POSITION`
event. The terminal's transaction queue is limited to 1024 records and events
may be out of order; the handler must copy and return, and any overflow GAP
must trigger full snapshot reconciliation. The reconciliation graph remains
the source of lifecycle state.

Manager callbacks are queued and dispatched by host-thread
`process_trade_events()`, outside runtime/Python mutexes. Both per-operation
callbacks and an optional global callback receive `TradeId`, `OperationId`,
`OperationState`, `TradeState`, `Certainty`, a monotonic revision, and the
current MT5-ID evidence snapshot. The queue is bounded: it must either
coalesce snapshots by logical ID or emit a `TRADE_EVENT_GAP` containing the
last delivered revision. The preferred contract is an explicit GAP followed by
an authoritative snapshot/query; state remains retained by TradeManager so a
consumer can recover without replaying every intermediate notification.

## Required test matrix

| Scenario | Required outcome |
| --- | --- |
| Immediate market result has order and deal | Bind observations, then reconcile |
| `deal == 0`, deal appears later | `accepted/reconciling` then `open/reconciled` |
| Deal precedes history order | Keep both; no false inconsistency |
| Timeout after server acceptance | Exactly one send; reconcile, never retry |
| Timeout with no unique candidate | `AMBIGUOUS`/`NOT_OBSERVED`, never `FAILED` or a guessed mapping; no retry |
| Two identical concurrent operations | Do not attribute fills without correlation |
| 40% + 60% partial fills | One `TradeId`, multiple deals, correct totals |
| Pending remainder then cancel | Preserve fills and terminal cancellation state |
| Netting two trades into one position | Attribution ledger or `AMBIGUOUS` after external mutation |
| Hedging independent positions | Keep position tickets/identifiers separate |
| Reversal (`DEAL_ENTRY_INOUT`) | Close old exposure and open new exposure as separate ledger edges |
| CloseBy (`ORDER_POSITION_BY_ID`/`DEAL_ENTRY_OUT_BY`) | Preserve both position links and validate both argument orders |
| Invalid filling/expiration policy | Reject before `order_send` |
| `order_check` succeeds but send requotes/rejects | Keep advisory check and classify the send result separately |
| Non-zero `retcode_external` | Preserve in raw result and diagnostics |
| Terminal restart | Rebuild graph from journal/snapshots; no duplicate send |
| Crash after durable `dispatching` but before send | Reconcile only; zero automatic resends |
| `DONE_PARTIAL` with IOC | Record fills and terminal cancelled remainder |
| `DONE_PARTIAL` with RETURN | Record fills and keep active remainder pending |
| Account switch before send/reconcile | Block send or emit `ACCOUNT_MISMATCH`; never mix graphs |
| Deadline before visibility | Emit `NOT_OBSERVED`, keep journal operation unresolved for later reconciliation |

## References and known quirks

See [mt5-quirks.md](mt5-quirks.md) for the linked MetaQuotes and forum
references covering asynchronous visibility, transaction ordering, result
ticket variability, position identifiers, filling modes, and history rewrites.

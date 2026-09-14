# Trade API contract

This document defines the trade layer planned above the existing MT5 Python
bridge. It records what can be proven from the terminal and what must remain
explicitly uncertain.

## Four implementation stages

1. **Raw typed observation:** `order_check`, active orders, positions, history
   orders, history deals, and symbol/account capabilities. This stage has no
   public side-effecting send method; an internal send primitive is used only
   after the journal barrier is delivered in stage 2.
2. **Durable dispatch and reconciliation:** persist the operation journal,
   execute one `order_send` behind the durable dispatch barrier, and use
   ticket-keyed snapshots and bounded overlap reads to build a graph of
   independently observed entities.
3. **TradeManager:** stable logical IDs, asynchronous worker delivery, and
   callbacks/state events.
4. **Optional native backend:** an MQL5 EA/service provides
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

## Identity model

The bridge owns two IDs:

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
changed. `TRADE_EVENT_GAP` (or another overflow reason) means a hint stream was
incomplete and an authoritative snapshot/query is required. These outcomes
may accompany `OperationState::reconciling` with provisional certainty, and an
unresolved `NOT_OBSERVED` operation can later become `CONFIRMED`,
`RECONCILED`, or `AMBIGUOUS`.

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
managed TradeManager uses a single-writer lease per AccountKey.

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

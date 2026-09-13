# Trade API contract

This document defines the trade layer planned above the existing MT5 Python
bridge. It records what can be proven from the terminal and what must remain
explicitly uncertain.

## Four implementation stages

1. **Raw typed access:** `order_check`, one-shot `order_send`, active orders,
   positions, history orders, history deals, and symbol capabilities.
2. **Reconciliation:** ticket-keyed snapshots and bounded overlap reads build a
   graph of independently observed entities.
3. **TradeManager:** stable logical IDs, asynchronous worker delivery, and
   callbacks/state events.
4. **Optional native backend:** an MQL5 EA/service provides
   `OrderSendAsync`/`OnTradeTransaction` records through IPC when the Python
   backend's synchronous call is insufficient.

The stages are deliberately ordered. A convenience `open_async()` built on a
single JSON `order_send` result would hide the hard cases instead of solving
them.

The first stage is intentionally “raw”: it must not synthesize a managed trade
from one result object. It includes the complete request/result fields and the
capability records needed to explain why a broker accepted, delayed, partially
filled, or rejected an operation.

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
backend can additionally correlate the request transaction with it.

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
`submitted/server_confirmed`, but it is not `open/reconciled` until a deal and
the relevant position evidence are observed.

## Submission and reconciliation algorithm

```text
generate TradeId + OperationId
capture before-snapshot (active orders, positions, relevant history)
resolve symbol execution/filling capabilities
order_check
persist durable dispatch intent (AccountKey + operation state)
order_send exactly once
persist raw result, retcode_external, and every non-zero ID
poll bounded snapshots of orders + deals + positions + history orders
reconcile by ticket/identifier graph and publish transitions
```

If `order_send` returns a timeout, the bridge performs no second send. It
reconciles the before/after observations and returns `PENDING`, `CONFIRMED`,
`NOT_OBSERVED`, or `AMBIGUOUS` with diagnostics. `NOT_OBSERVED` means only
that no evidence was visible before the deadline; it does not prove the server
did not execute the request and never permits a retry. Two concurrent
identical requests with no correlation evidence must remain `AMBIGUOUS`;
matching by symbol/volume/time, comment, or magic cannot safely choose one.

Before the side effect, the operation journal durably records its
`AccountKey` (at minimum server and login), `TradeId`, `OperationId`, request
payload, and `dispatch_intent_persisted`. After the call it records the raw
result before moving to `reconciling`. On restart, a `dispatching` record with
no result is reconciled from snapshots and is never resent.

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
  volume min/max/step/limit, and tick size. Account capabilities include
  margin mode, FIFO-close, hedge permission, and trade permission.
- **External changes:** manual trades, SL/TP, expiration, and broker/exchange
  execution differences are recorded as graph observations, not silently
  attributed to a bridge operation.

## Backend boundary

The current Python package has synchronous `order_check`, `order_send`,
`history_orders_get`, `history_deals_get`, `orders_get`, and `positions_get`.
It does not expose terminal `OrderSendAsync` or `OnTradeTransaction`. The
first production manager can therefore be asynchronous to the C++ caller by
running these calls on a dedicated worker and delivering bounded events, but
it is not terminal-native async.

`order_check` is advisory: a successful check does not reserve price,
liquidity, or permissions for the subsequent send. Return codes are
operation-aware: `PLACED` is a valid pending-order result, `DONE_PARTIAL` is
not a terminal fill, and `TIMEOUT`, `REQUOTE`, and `REJECT` remain distinct.

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
current MT5-ID evidence snapshot.

## Required test matrix

| Scenario | Required outcome |
| --- | --- |
| Immediate market result has order and deal | Bind observations, then reconcile |
| `deal == 0`, deal appears later | `submitted/reconciling` then `open/reconciled` |
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

## References and known quirks

See [mt5-quirks.md](mt5-quirks.md) for the linked MetaQuotes and forum
references covering asynchronous visibility, transaction ordering, result
ticket variability, position identifiers, filling modes, and history rewrites.

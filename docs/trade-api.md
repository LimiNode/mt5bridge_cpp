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

## Identity model

The bridge owns two IDs:

- `TradeId` — one logical lifecycle, possibly including open, partial fills,
  modifications, and close.
- `OperationId` — one side effect such as open, cancel, modify, or close.

MT5 identifiers are stored as separate optional evidence:

```text
TradeId
  ├── OperationId[]
  │     └── request_id?       (native async backend only)
  ├── order_ticket[]
  ├── deal_ticket[]
  ├── position_identifier[]
  └── position_ticket[]
```

`POSITION_IDENTIFIER` links order/deal history to a position; it is not the
same field as `POSITION_TICKET`. Neither is the bridge's `TradeId`.

## State and certainty

The lifecycle state is not a binary success flag:

```text
queued -> prechecking -> submitting -> submitted -> reconciling
                                      ├── pending
                                      ├── partially_filled -> filled/open
                                      ├── rejected/failed
                                      ├── cancelled/expired
                                      └── ambiguous
open -> close_requested -> partially_closed -> closed
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
order_send exactly once
persist raw result, retcode_external, and every non-zero ID
poll bounded snapshots of orders + deals + positions + history orders
reconcile by ticket/identifier graph and publish transitions
```

If `order_send` returns a timeout, the bridge performs no second send. It
reconciles the before/after observations and returns `PENDING`, `CONFIRMED`,
`NOT_FOUND`, or `AMBIGUOUS` with diagnostics. Two concurrent identical
requests with no correlation evidence must remain `AMBIGUOUS`; matching by
symbol/volume/time, comment, or magic cannot safely choose one.

History reads use sets/maps keyed by tickets and bounded overlap snapshots.
They never rely on chronological order, the last array element, or an
append-only assumption. A deal may be visible before its history order, and a
history order may appear in a different position after a refresh.

## Account and execution semantics

- **Hedging:** multiple independent positions may correspond to one logical
  trade or operation.
- **Netting:** several logical trades can contribute to one symbol position.
  Closing one `TradeId` therefore uses an attribution ledger; if an external
  mutation makes the remaining volume unknowable, report `AMBIGUOUS`.
- **Partial fills:** one logical operation can produce multiple deals and a
  pending remainder. Do not replace the deal list with one “final ticket”.
- **Pending/limit orders:** retain the order while fills arrive; cancellation
  preserves already filled volume.
- **Execution/filling modes:** inspect symbol capabilities and resolve a
  compatible `FOK`, `IOC`, or `RETURN` policy before `order_check`/send.
- **External changes:** manual trades, SL/TP, expiration, and broker/exchange
  execution differences are recorded as graph observations, not silently
  attributed to a bridge operation.

## Backend boundary

The current Python package has synchronous `order_send`,
`history_orders_get`, `history_deals_get`, `orders_get`, and `positions_get`.
It does not expose terminal `OrderSendAsync` or `OnTradeTransaction`. The
first production manager can therefore be asynchronous to the C++ caller by
running these calls on a dedicated worker and delivering bounded events, but
it is not terminal-native async.

An optional MQL5 backend may later forward only copied transaction records into
an IPC ring. Its `request_id` is valid for the request transaction, not as a
universal ID for every later `ORDER_*`, `DEAL_*`, `HISTORY_*`, or `POSITION`
event. The reconciliation graph remains the source of lifecycle state.

## Required test matrix

| Scenario | Required outcome |
| --- | --- |
| Immediate market result has order and deal | Bind observations, then reconcile |
| `deal == 0`, deal appears later | `submitted/reconciling` then `open/reconciled` |
| Deal precedes history order | Keep both; no false inconsistency |
| Timeout after server acceptance | Exactly one send; reconcile, never retry |
| Timeout with no unique candidate | `AMBIGUOUS`, never `FAILED` or a guessed mapping |
| Two identical concurrent operations | Do not attribute fills without correlation |
| 40% + 60% partial fills | One `TradeId`, multiple deals, correct totals |
| Pending remainder then cancel | Preserve fills and terminal cancellation state |
| Netting two trades into one position | Attribution ledger or `AMBIGUOUS` after external mutation |
| Hedging independent positions | Keep position tickets/identifiers separate |
| Invalid filling policy | Reject before `order_send` |
| Non-zero `retcode_external` | Preserve in raw result and diagnostics |
| Terminal restart | Rebuild graph from journal/snapshots; no duplicate send |

## References and known quirks

See [mt5-quirks.md](mt5-quirks.md) for the linked MetaQuotes and forum
references covering asynchronous visibility, transaction ordering, result
ticket variability, position identifiers, filling modes, and history rewrites.

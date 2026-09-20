# ADR-0004: Trade identity and reconciliation are explicit layers

- Status: proposed
- Date: 2026-09-14

## Context

MetaTrader 5 does not expose trading as one atomic, immediately consistent
state. `order_send()` may return after the server accepted a request, while
active orders, positions, deals, and history orders become visible at
different times. A deal can appear before its history order, history results
are not a reliable append-only queue, and a market result may contain `order`
or `deal` equal to zero depending on execution mode and broker behavior. A
process can also fail after the server accepted a request but before an in
memory result was recorded, so restart recovery requires a durable intent.

The Python package exposes synchronous `order_send()` and read APIs, but does
not expose terminal `OrderSendAsync` or `OnTradeTransaction`. The bridge must
therefore not pretend that one Python result is a complete trade lifecycle.
The detailed contract and test matrix live in
[trade-api.md](../trade-api.md).

## Decision

Trading is developed in four layers:

1. **Typed raw observation API.** Expose normalized requests/results and
   read-only access to `order_check`, active orders, positions, history orders,
   history deals, and symbol/account capabilities. Preserve all broker
   identifiers and raw result fields; do not infer a position from one ticket
   field. Store `request_id` whenever MT5 returns it, including from a Python
   `order_send()` result; in the Python backend it is evidence only, while a
   native transaction backend can use it for request-event correlation. Stage 1
   does not expose a side-effecting send method: the internal send primitive is
   introduced only behind the durable journal barrier in stage 2.
2. **Durable dispatch and reconciliation engine.** Add the write-ahead journal,
   execute the one send only after its durable `dispatching` barrier, and build
   a graph from independently observed requests, orders, deals, positions, and
   history records. Re-read bounded overlaps and index entities by
   ticket/identifier sets, never by array order or an assumed append position.
   Handle delayed visibility, partial fills, rewrites, and missing links
   explicitly.
3. **High-level `TradeManager`.** Give applications stable logical
   `TradeId` and per-side-effect `OperationId` values. An asynchronous worker
   may call the synchronous Python API and publish state transitions through a
   bounded callback/event queue, while the caller remains independent of MT5
   tickets.
4. **Optional terminal-native backend.** If true asynchronous submission is
   required, a small MQL5 EA/service may use `OrderSendAsync` and copy
   `OnTradeTransaction` records into an IPC ring. It is a separate backend
   behind the same logical model, not a second implementation of business
   rules.

After generating `TradeId` and `OperationId`, capturing a baseline, resolving
symbol capabilities, and running `order_check`, every side-effecting operation
follows this journal sequence:

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

The journal state is `created -> prechecked -> dispatch_intent_persisted ->
dispatching -> result_persisted -> reconciling`. `TradeId` and `OperationId`
are stable across process restarts and the journal is namespaced by
`AccountKey` (at minimum server plus login). `dispatch_intent_persisted` is a
pre-side-effect intent: recovery may still acquire the same lease, verify the
account, durably advance to `dispatching`, and perform the one send. Once
`dispatching` is durable, it means the side effect may have begun; recovery
only reconciles snapshots and never resends it. Anything after that point is
at-most-once and permanently non-resendable by automatic recovery, including a
crash after the barrier but before entering `order_send()`. An `order_send()`
timeout or IPC failure never triggers a blind second send. The result is
provisional until reconciliation proves what happened.

`AccountKey` is immutable for an operation and scopes every graph key as
`(AccountKey, OrderTicket)`, `(AccountKey, DealTicket)`, and
`(AccountKey, PositionIdentifier)`. It consists of the terminal `server` and
`login`; trade permissions are capabilities, not identity fields. Immediately
before every side effect the current terminal account must equal the operation
key, the required account capabilities must permit the request, and the
process must hold the matching single-writer lease. Otherwise the operation is
not sent or advanced to `dispatching`. If the
account changes during reconciliation, observation is suspended and the
operation receives `ACCOUNT_MISMATCH` rather than attaching the new account's
records to the old graph. A managed TradeManager holds a single-writer lease
per AccountKey so two processes cannot maintain competing netting ledgers. The
lease must be an OS-backed exclusive ownership primitive held continuously
through the dispatch barrier and `order_send`, or a fencing-token protocol;
a bare time-based lease without fencing is insufficient because an expired
owner could continue sending after a new owner takes over.

## Identity and certainty rules

`TradeId` identifies the logical lifecycle (open, partial fills, modify, and
close). `OperationId` identifies one side effect within that lifecycle. They
are generated by the bridge and are not aliases for MT5 values.

The bridge records `request_id` whenever present, as well as order tickets,
deal tickets, position tickets, and `POSITION_IDENTIFIER` values. These are
evidence in a graph:

```text
TradeId -> OperationId[] -> request_id?
                   |-> order_ticket[]
                   |-> deal_ticket[]
                   |-> position_identifier[]
                   `-> position_ticket[]
```

`request_id` correlates the request transaction when a transaction stream is
available; it does not identify every subsequent transaction. The bridge stores
it with an explicit `TerminalSessionId` and never correlates it across terminal
sessions or restarts. The current Python backend does not receive
`OnTradeTransaction`, so comments and magic numbers are additional evidence,
never identity. `result.order` and `result.deal` are optional observations,
not universal guarantees.

Operation state, logical Trade state, and certainty are separate dimensions.
Operation state includes `queued`, `prechecking`, `submitting`, `accepted`,
`reconciling`, `partially_filled`, `filled`, `cancelled`, `expired`, `rejected`,
`failed`, and `ambiguous`. Trade state includes `pending`, `partially_open`,
`open`, `reducing`, `closing`, `closed`, `reversed`, and `ambiguous`. Certainty is
`provisional`, `server_confirmed`, `reconciled`, or `ambiguous`. A filled close
operation can therefore leave a trade `reducing` or `closed`; `filled` never
means that the logical trade is open.
`accepted` is used only when MT5 provides server-acceptance evidence; a timeout
may move directly from `submitting` to `reconciling` with provisional
certainty.

Reconciliation outcomes and reasons are metadata alongside those three
dimensions; they are not a fourth lifecycle state. `PENDING` means the
operation is still being observed, `CONFIRMED` means matching server evidence
has been found, `NOT_OBSERVED` means the deadline passed without evidence, and
`ACCOUNT_MISMATCH` means observation was stopped because the terminal account
changed, and `AMBIGUOUS` means no unique attribution could be proven.
`TRADE_EVENT_GAP` (or another overflow reason) means that a hint stream was
incomplete and an authoritative snapshot is required. These outcomes may
accompany `OperationState::reconciling` with provisional certainty. An
unresolved `NOT_OBSERVED` operation can later resolve as
`ReconciliationOutcome::CONFIRMED` with `Certainty::reconciled`, or as
`ReconciliationOutcome::AMBIGUOUS` with `Certainty::ambiguous`.

The canonical outcome vocabulary is:

```text
ReconciliationOutcome:
PENDING, CONFIRMED, NOT_OBSERVED, ACCOUNT_MISMATCH, TRADE_EVENT_GAP, AMBIGUOUS
```

## Safety constraints

- The initial reconciliation wait is bounded by a deadline and reports attempts, last MT5
  error, observed records, and the reason for `PENDING`, `NOT_OBSERVED`, or
  `AMBIGUOUS`. `NOT_OBSERVED` means only that no matching evidence was visible
  before the deadline; it never proves that the server did not execute the
  operation and never permits an automatic retry. The operation remains
  unresolved in the journal; later snapshots, reconnects, history refreshes,
  or a restart may transition it to `CONFIRMED`, `RECONCILED`, or `AMBIGUOUS`.
- Ticket-keyed snapshots and overlap reconciliation are required; chronology
  and “last array element” assumptions are forbidden.
- The graph retains `DEAL_ORDER`, `DEAL_POSITION_ID`, `DEAL_ENTRY`,
  `DEAL_REASON`, `ORDER_POSITION_ID`, `ORDER_POSITION_BY_ID`, `ORDER_REASON`,
  `POSITION_TICKET`, `POSITION_IDENTIFIER`, `POSITION_REASON`, and
  exchange-provided order/deal/position external IDs when available. These
  fields explain partial fills, reversals, SL/TP/StopOut, and CloseBy links.
- `POSITION_TICKET` and `POSITION_IDENTIFIER` remain distinct. Hedging keeps
  independent positions; netting may combine multiple logical trades into one
  position, which requires an explicit virtual-lot attribution ledger. The
  manager may be configured for FIFO, LIFO, or pro-rata allocation, or require
  explicit lots; the default is to reject an unconfigured netting close as
  `AMBIGUOUS`. External/manual/SL/TP mutations make attribution ambiguous
  rather than silently assigning volume.
- Partial fills are one logical trade with multiple deal records and possibly
  a pending remainder. Filling mode (`FOK`, `IOC`, `RETURN`) is resolved from
  symbol capabilities before sending; `BOC` is also supported for limit and
  stop-limit requests. Expiration/GTC mode, stops/freeze levels, volume
  min/max/step/limit, tick size, and `SYMBOL_ORDER_CLOSEBY` are part of symbol
  capabilities; account margin mode, FIFO-close, hedge permission,
  `ACCOUNT_TRADE_ALLOWED`, `ACCOUNT_TRADE_EXPERT`, and trade permission are
  part of account capabilities. Invalid combinations are rejected.
- `order_check` is advisory only: a successful check does not guarantee the
  later `order_send` will succeed. Return codes are classified by operation;
  `PLACED` is success for a pending order, and `DONE_PARTIAL` is not by itself
  a terminal operation; `TIMEOUT`, `REQUOTE`, and `REJECT` have distinct
  outcomes.
- `TRADE_RETCODE_MARKET_CLOSED` (`10018`) from `order_send` is a deterministic
  broker rejection even when `order_check` returned `Done`; persist the full
  result, mark the operation `rejected`, and do not retry or classify it as
  transport ambiguity.
- The raw result preserves `retcode`, `retcode_external`, `order`, `deal`, and
  all other fields returned by MT5. No field is promoted to a universal
  position or completion identifier.
- `DONE_PARTIAL` proves only partial execution. Operation terminality depends
  on the operation kind, filling policy, execution mode, and the reconciled
  active-order remainder: with `IOC` the remainder may already be cancelled,
  while with `RETURN` it may remain active. A partial result is therefore not
  universally terminal or universally pending.
- A transaction stream is a hint stream. An MQL `OnTradeTransaction` handler
  must copy events into a bounded ring and return immediately; its 1024-entry
  terminal queue can overflow or deliver events out of order. Any overflow or
  GAP triggers full snapshot reconciliation.

The JSON control plane can carry the first additive raw/reconciliation methods.
If a high-volume typed trade surface is added later, it must use a new
versioned POD ABI; STL, Python objects, and third-party allocators do not cross
the DLL boundary.

## Consequences

Applications can recover an uncertain submission without creating a duplicate
order, and can distinguish a confirmed fill from a merely accepted request.
They must handle `PENDING`, `NOT_OBSERVED`, and `AMBIGUOUS`; the bridge cannot
invent an idempotency key or infer attribution where MT5 provides none.

The implementation is intentionally staged. The raw API and fake-runtime
model must land before a high-level manager. A real-terminal acceptance suite
must cover delayed visibility, partial fills, netting/hedging, terminal restart,
and ambiguous concurrent identical requests without sending duplicates.

Callbacks for a future manager are delivered on the host owner loop through a
bounded `process_trade_events()` queue. Per-operation callbacks and an optional
global callback are invoked outside runtime/Python mutexes; each event carries
`TradeId`, `OperationId`, the three state dimensions, a monotonic revision, and
the current MT5-ID evidence snapshot. Worker-process isolation remains a
separate reliability/backend decision. It must preserve the same logical IDs,
no-blind-retry rule, and reconciliation semantics.

## References

- [MQL5 trade request result](https://www.mql5.com/en/docs/constants/structures/mqltraderesult)
- [MQL5 `OnTradeTransaction`](https://www.mql5.com/en/docs/event_handlers/ontradetransaction)
- [MQL5 `order_send` Python API](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersend_py)
- [MQL5 history selection by position identifier](https://www.mql5.com/en/docs/trading/historyselectbyposition)
- [MQL5 symbol order/filling modes](https://www.mql5.com/en/docs/constants/environment_state/marketinfoconstants)
- [MQL5 forum: trade synchronization](https://www.mql5.com/en/forum/393733/page1723)
- [MQL5 forum: non-chronological history orders](https://www.mql5.com/en/forum/454011)

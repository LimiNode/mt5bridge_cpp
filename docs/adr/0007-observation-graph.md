# ADR-0007: Account-scoped observation graph

## Status

Accepted. This slice implements the read-only graph foundation after typed
trade snapshots and before the durable dispatch journal.

## Decision

`mt5bridge::ObservationGraph` is a lightweight C++ consumer-side graph. It
accepts an `ObservationBatch` containing one `AccountKey` and any combination
of active orders, positions, history orders, and history deals. It never calls
MetaTrader, owns a runtime, or submits `order_send`.

The account key is `(server, login)`. An unbound graph adopts the first valid
batch; a bound graph rejects a different account atomically with
`account_mismatch`. A batch with an empty server or a zero primary ticket is
rejected without changing the graph or its revision.

Evidence is upserted by its primary MT5 ticket, while active orders and history
orders remain separate namespaces. Position `ticket` and `identifier` remain
distinct. Read methods return deterministic ticket-sorted copies and expose
only explicit links:

```text
ORDER_POSITION_ID / POSITION_IDENTIFIER
DEAL_ORDER         -> history order or active order ticket
DEAL_POSITION_ID   -> position identifier
```

Every accepted batch advances a monotonic revision, including an empty
observation. `clear_evidence()` removes records but retains the account scope.

The graph deliberately does not invent `TradeGroupId`, `TradeId`,
`OperationId`, `request_id`, certainty, or lifecycle state. Those are managed
domain/journal concepts and may only be associated by a later reconciliation
layer with explicit evidence and account/session scope.

## Consequences

- Reordered MT5 arrays cannot change graph identity or link results.
- Account switches cannot mix evidence from two terminals.
- A later journal/reconciliation worker can consume a deterministic snapshot
  graph without adding another Python or C ABI implementation.
- Persistence, deadlines, transaction hints, and side effects remain outside
  this header-only foundation and are intentionally deferred to the next
  stage.

## Verification

`tests/reconciliation_graph_test.cpp` covers first-account binding, sorted
links, same-ticket upsert, account mismatch isolation, invalid primary IDs, and
revision/clear semantics.

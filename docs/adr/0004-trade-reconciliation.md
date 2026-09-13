# ADR-0004: Explicit trade reconciliation after side-effecting calls

- Status: accepted
- Date: 2026-09-14

## Context

`MetaTrader5.order_send()` is a side-effecting operation. A timeout or IPC
failure can happen after the trade server has accepted the request, so sending
the same order again is not safe. A non-null result is also not by itself a
complete application-level confirmation: orders, deals, and positions can
become mutually visible only after a short synchronization delay.

## Decision

The bridge keeps order submission and reconciliation as separate operations:

1. `open_market_buy` performs exactly one `order_send()` call and returns the
   raw MetaTrader result. It never retries automatically.
2. A future additive JSON operation, `reconcile_trade`, is read-only and uses
   caller-supplied identifiers (order/deal/position tickets) whenever
   available. It polls the relevant MT5 history/account queries until a
   caller-provided deadline and returns an explicit state:
   `CONFIRMED`, `PENDING`, `NOT_FOUND`, `AMBIGUOUS`, or `ERROR`.
3. Symbol/volume/time-window matching is diagnostic only. It must not silently
   select one candidate or authorize a second order when multiple candidates
   match.
4. Reconciliation is bounded and reports the last MT5 error, attempts, and
   the records observed. It does not mutate orders and does not retry the
   original side effect.

The JSON control plane remains sufficient for this additive operation; no C
ABI layout change is required. If a typed high-volume trade result is needed
later, it must use new versioned POD records rather than Python or STL types.

## Consequences

Applications can recover from an uncertain `order_send()` outcome without
creating a duplicate order, while callers still decide what to do with
`PENDING`, `NOT_FOUND`, and `AMBIGUOUS`. The bridge cannot manufacture an
idempotency key that MetaTrader does not provide, so reconciliation by broad
market attributes remains inherently weaker than ticket-based confirmation.

The implementation must be covered by fake-runtime tests for delayed
visibility, no candidate, and ambiguous candidates before it is exposed in the
Python adapter. A real-terminal acceptance test should verify the bounded
deadline and the order/deal/position visibility sequence without submitting
duplicate orders.

Worker-process isolation and automatic restart remain separate concerns. They
may change runtime availability, but must preserve this no-duplicate-side
effect contract.

# ADR-0029: Provenance-bearing durable exit settlement

## Status

Accepted as the bounded follow-up to the durable operation-kind discriminator.

## Decision

Managed owner preparation persists `OperationKind::close` and
`OperationKind::cancel` explicitly. The owner never infers exit semantics from
request payload bytes or from a generic lifecycle state.

An exit `close` operation settles only after a fresh reconciliation cycle proves
the result-derived history-deal identity and attributes one order/position
identity. The settlement aggregator accepts only `DEAL_ENTRY_OUT` and
`DEAL_ENTRY_OUT_BY` deals; `DEAL_ENTRY_INOUT` is rejected for this slice because
its reversal volume cannot be split into close and new-entry semantics without a
separate attribution policy. The attributed logical volume is committed as the
record's durable `settled_volume` and is applied as a reduction of confirmed
open exposure. Partial close is terminal for that operation and leaves the
close obligation active so a new operation id can target the remaining exposure.

A `cancel` operation settles only after a fresh authoritative observation proves
the bound active order is absent. It clears pending entry remainder while
preserving all already confirmed open volume. No cancel volume is recorded as
executed exposure.

Restart reconstruction replays durable `open`, `cancel`, and `close` records in
operation-id order. Missing operation ids, `unspecified` kinds, kind/state
mismatches, impossible volumes, and invalid provenance fail closed. The file
store rejects CAS updates that attempt to rewrite an existing operation kind.

## Consequences

- Close and cancellation are explicit durable capabilities, not payload guesses.
- Partial close reduces exposure without manufacturing a new entry.
- Cancellation cannot erase previously confirmed fills.
- Reversal (`INOUT`), netting attribution, and public `TradeManager` policy remain
  outside this bounded slice.

## Verification

Owner-loop regressions cover partial close, full close, cancellation with a
pending remainder, and restart replay of OPEN → CANCEL → CLOSE. Exit tests use
fresh history provenance and reject entry-direction deals. Journal storage tests
cover immutable operation-kind compare-and-commit behavior.

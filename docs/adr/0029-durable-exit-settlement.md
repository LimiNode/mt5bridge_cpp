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
separate attribution policy. The same fail-closed rule applies when an INOUT
deal is encountered alongside otherwise attributable history rows. The
attributed logical volume is committed as the
record's durable `settled_volume` and is applied as a reduction of confirmed
open exposure. A partial close is not terminal for that operation: its durable
record remains `partially_filled`, and later reconciliation re-aggregates the
cumulative attributed exit volume and applies only the strictly new delta. The
same operation becomes terminal only when cumulative volume reaches its
requested volume; only then may a new operation id target remaining exposure.

A `cancel` operation settles only after a fresh authoritative observation proves
the bound active order is absent and the requested history-deal window is fresh
and covered. The owner re-aggregates the cumulative entry volume for the
durable OPEN identity and applies any late-fill delta before clearing the
remaining pending entry volume. Thus active-order disappearance alone cannot
erase a fill that became visible after the original partial OPEN settlement.
No cancel volume is recorded as executed exposure.

Restart reconstruction replays durable `open`, `cancel`, and `close` records in
operation-id order. Missing operation ids, `unspecified` kinds, kind/state
mismatches, impossible volumes, and invalid provenance fail closed. The file
store rejects CAS updates that attempt to rewrite an existing operation kind.

## Consequences

- Close and cancellation are explicit durable capabilities, not payload guesses.
- Partial close reduces exposure without manufacturing a new entry and remains
  unresolved until cumulative terminality is proven on the same operation.
- Cancellation cannot erase previously confirmed fills or late fills that are
  proven by fresh history coverage.
- Reversal (`INOUT`) implementation and netting attribution remain outside
  this bounded slice; their durable proof boundary is defined in
  [ADR-0030](0030-durable-reversal-attribution-model.md). Public
  `TradeManager` policy remains deferred as well.

## Verification

Owner-loop regressions cover partial close followed by a late fill on the same
operation, replacement-close rejection until terminality, cancellation with a
pending remainder plus a late entry fill, and restart replay of OPEN -> CANCEL ->
CLOSE. Exit tests use fresh history provenance and reject entry-direction deals.
Journal storage tests cover immutable operation-kind compare-and-commit behavior.

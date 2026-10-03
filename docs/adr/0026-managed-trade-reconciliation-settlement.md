# ADR-0026: Provenance-bearing managed-trade settlement

## Status

Accepted as the bounded settlement slice after the private managed-trade owner
loop. This remains private C++ infrastructure; it does not introduce a public
`TradeManager` or a new broker-call surface.

## Decision

`dispatch::ManagedTradeOwner::settle_reconciliation()` accepts an
`OperationReconciliationWorker`, not broker evidence, a fill volume, or a
caller-selected lifecycle outcome. The owner asks that worker to perform one
authoritative observation cycle and may mutate `ManagedTradeState` only when
the returned cycle carries all of the following:

- an accepted `ObservationSample` from the worker's coordinator;
- matching account, graph instance, and evaluated graph revision;
- a durable journal record whose key, revision, and lifecycle state match the
  worker cycle; and
- a post-baseline graph revision newer than the worker's effective baseline in
  the same graph instance; numeric revisions from another process are never
  compared.

For an `OPEN` slice, the bounded semantic settlement path additionally
requires a fresh `history_deal_present` predicate with a valid inclusive time
window. The predicate's broker ticket must be either the durable ticket or a
single-assignment result binding. The owner uses that ticket only as an anchor:
it attributes all fresh history deals linked to the anchor's `DEAL_ORDER`,
requires known `DEAL_ENTRY`, `DEAL_VOLUME`, and `DEAL_TIME`, ignores exit deals,
and sums the remaining entry (`IN`/`INOUT`) volumes. Thus a raw broker deal
ticket, active-order presence, or one arbitrary history row cannot settle the
operation by itself. The logical managed-trade volume is intentionally an
integer in this slice; a non-integral or otherwise malformed broker total is
left unresolved rather than rounded.

When the attributed total is positive but below the requested slice volume,
the owner commits `partially_filled`, preserving the remainder. When it is at
least the requested volume, it commits `filled` and caps the recorded result
at the requested amount. Both the managed state and the durable journal are
advanced only after the candidate state validates and the journal transition
commits. A confirmed predicate without this history-deal provenance leaves
both in `reconciling`.

An authoritatively ambiguous cycle may record `unknown` only when the same
provenance checks pass. Pending, not-observed, and event-gap cycles leave the
managed slice unresolved. Account mismatch, missing provenance, and durable
identity mismatch cannot settle the managed state.

Remainder handling after the partial result, close/cancel settlement, late
fills, and restart reconstruction of aggregate exposure remain later bounded
slices. The worker and owner therefore keep the durable journal non-resendable
while those semantics are absent.

## Consequences

- Raw broker result payloads remain hints and cannot directly settle managed
  exposure.
- The only settlement input is a fresh, provenance-bearing observation cycle
  tied to the durable operation descriptor.
- The owner and reconciliation worker cannot silently disagree about the
  operation identity or graph revision.
- A pending observation does not change managed exposure, and an ambiguous
  observation never becomes a resend capability.
- The public SDK, C ABI, Python runtime, and public `TradeManager` remain
  unchanged.

## Verification

`tests/managed_trade_owner_test.cpp` proves that a real worker observation of a
new active order does not imply a full fill: managed volume stays zero and the
journal remains non-terminal. It also covers a pending cycle: the journal is
normalized for reconciliation while managed state stays `submitting` and no
second backend call is possible. The history-deal regressions bind a broker
deal identity, aggregate multiple fresh entry deals by their order, ignore an
exit deal, and verify both full and partial transitions with a preserved
remainder.

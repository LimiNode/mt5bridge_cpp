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
requires exactly one fresh `history_deal_present` predicate with a valid
inclusive time window. That deal anchor must have an empty descriptor ticket,
a non-zero correlation id, and exactly one durable result-derived binding for
that correlation. A caller-chosen deal ticket is never semantic settlement
evidence. The owner uses the bound ticket only as an anchor: it attributes all
fresh history deals linked to the anchor's `DEAL_ORDER`, requires known
`DEAL_ENTRY`, `DEAL_VOLUME`, and `DEAL_TIME`, ignores exit deals, and sums the
remaining entry (`IN`/`INOUT`) volumes. Thus active-order presence, an
unlinked arbitrary history row, or a pre-bound deal ticket cannot settle the
operation by itself. The logical managed-trade volume is intentionally an
integer in this slice; a non-integral or otherwise malformed broker total is
left unresolved rather than rounded.

When the attributed total is positive but below the requested slice volume,
the owner commits `partially_filled`, preserving the remainder. When it is
exactly the requested volume, it commits `filled`. An aggregate above the
requested volume is anomalous and remains unresolved; evidence is never capped
to manufacture a normal fill. Both the managed state and the durable journal
are advanced only after the candidate state validates and the journal
transition commits. A confirmed predicate without this history-deal
provenance leaves both in `reconciling`.

An authoritatively ambiguous cycle may record `unknown` only when the same
provenance checks pass. Pending, not-observed, and event-gap cycles leave the
managed slice unresolved. Account mismatch, missing provenance, and durable
identity mismatch cannot settle the managed state.

A partial OPEN with a pending remainder remains eligible for a fresh
`OperationReconciliationWorker` cycle through
`ManagedTradeOwner::settle_pending_remainder()`. The owner requires a complete
fresh-covered deal history window and re-aggregates the authoritative
cumulative entry volume linked to the durable result binding. It compares that
cumulative frontier with durable `settled_volume` and advances only the delta.
The delta is added to both managed `open_volume`/`slice.result_volume` and durable
`settled_volume` in one bounded update. A partial-to-partial update is therefore
allowed only when the cumulative settled volume strictly increases; the final
remainder transitions the same OPEN slice to `filled`. Repeated full history
snapshots and a new graph after restart use the same durable frontier and
cannot re-count an earlier deal. Contradictory evidence in a remainder cycle
leaves the confirmed partial exposure and durable partial record intact so a
later fresh worker can retry.

Close/cancel settlement remains a later bounded slice. Restart reconstruction
for settled OPEN records is defined separately in
[ADR-0027](0027-managed-trade-restart-reconstruction.md);
unresolved, close, and cancel records remain non-resendable until their own
recovery semantics exist. The worker and owner therefore keep the durable
journal non-resendable while those semantics are absent.

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
exit deal, and verify full, partial, late-remainder, cumulative durable-volume,
and ambiguous-retry transitions.

# ADR-0027: Restart reconstruction of settled managed OPEN slices

## Status

Accepted as the bounded follow-up to the provenance-bearing OPEN settlement
slice. This remains private C++ infrastructure; it does not introduce a
public `TradeManager`, a new broker-call surface, or close/cancel recovery.

## Decision

The durable journal records the managed logical volume requested by an OPEN
slice and the authoritative volume proved when that slice settles. The journal
format advances to version 3 while readers continue to accept versions 1 and
2; older records remain valid but do not claim reconstructable managed volume.

`ManagedTradeOwner::recover_settled_open()` accepts an initialized empty state
carrying the trade's configured bounds and the complete set of recovered
operation records. It replays only records for that account and trade whose
journal state is `reconciling`, whose operation state is `filled` or
`partially_filled`, and whose requested/settled volumes are durable and
consistent. Each replayed operation must be the next operation id, must be an
OPEN-sized slice within the configured limit, and must reproduce the expected
managed state transition. Any gap, malformed record, in-flight operation, or
inconsistent volume fails closed.

The recovery helper does not accept caller-supplied fill evidence. It replays
only volumes previously committed by the owner after provenance-bearing
history-deal settlement. Unresolved records are not converted into exposure or
into a resend capability; their separate observation-only recovery remains the
responsibility of the journal/reconciliation worker.

## Consequences

- A full OPEN settlement restores `open_volume` after process restart.
- A partial OPEN settlement restores both confirmed exposure and its pending
  remainder.
- Durable managed volume is no longer inferred from opaque request/result
  payloads.
- Legacy records without the new fields are read conservatively and cannot be
  used by this reconstruction helper until a fresh authoritative settlement is
  committed.
- Close/cancel, late-fill aggregation, and reconstruction of an unresolved
  operation remain later bounded slices.

## Verification

The managed-owner regression reconstructs full and partial OPEN records through
a new `OperationJournal` instance and checks exposure/remainder invariants. The
file-store regression reopens a version-3 record and verifies that requested and
settled managed volumes survive serialization.

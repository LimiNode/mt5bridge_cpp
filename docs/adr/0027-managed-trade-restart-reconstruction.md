# ADR-0027: Restart reconstruction of settled managed OPEN slices

## Status

Accepted as the bounded follow-up to the provenance-bearing OPEN settlement
slice. This remains private C++ infrastructure; it does not introduce a
public `TradeManager`, a new broker-call surface, or close/cancel recovery.

## Decision

The durable journal records the immutable operation kind, the managed logical
volume requested by an OPEN slice, and the authoritative volume proved when
that slice settles. The journal format advances to version 4 while readers
continue to accept versions 1 through 3. Version-3 records with managed
requested-volume metadata are conservatively migrated as `open`; older records
without that metadata remain kind-unspecified and do not claim reconstructable
managed volume.

`ManagedTradeOwner::recover_settled_open()` accepts an initialized empty state
carrying the trade's configured bounds and a complete, accepted journal scan.
It replays only records for that account and trade whose
journal state is `reconciling`, whose operation state is `filled` or
`partially_filled`, and whose requested/settled volumes are durable and
consistent. Each replayed operation must be the next operation id, must be an
durable `open` slice within the configured limit, and must reproduce the expected
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
- Legacy records without the new fields are read conservatively. Version-3
  settled records retain OPEN reconstruction through an inferred `open` kind;
  version-1/2 records remain kind-unspecified and cannot be used by this
  reconstruction helper until a fresh authoritative settlement is committed.
- Close/cancel settlement and reconstruction of an unresolved operation remain
  later bounded slices. A partial OPEN may first advance through the bounded
  late-remainder path in ADR-0026; restart then replays its cumulative durable
  `settled_volume` just like any other settled OPEN record.
- Durable operation kind is now persisted in version 4. `close` and `cancel`
  records are recoverable as distinct operation semantics but are not yet
  applied by `recover_settled_open()`; their settlement remains a later bounded
  slice. This prevents a future close fill from being mistaken for an OPEN fill
  and incorrectly increasing managed exposure.

## Verification

The managed-owner regression reconstructs full and partial OPEN records through
a new `OperationJournal` instance, rejects a durable close record in the OPEN
reconstructor, and checks exposure/remainder invariants. The file-store
regression reopens version-3 and version-4 records and verifies conservative
migration, operation-kind persistence, and requested/settled managed volumes.

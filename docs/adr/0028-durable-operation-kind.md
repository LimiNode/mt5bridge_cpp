# ADR-0028: Durable operation-kind discriminator

## Status

Accepted as the bounded journal-format follow-up to the managed OPEN remainder
and restart-reconstruction slices.

## Decision

Every new journal record carries an immutable `operation_kind` discriminator:

```text
unspecified  generic/legacy record with no managed settlement semantics
open         entry side effect that may increase exposure
close        exit side effect that may reduce exposure
cancel       cancellation of an outstanding broker-side remainder
```

The file journal advances to format version 4 and persists the discriminator
next to the durable lifecycle and settlement fields. The existing two-argument
`OperationJournal::create()` remains source-compatible and creates an
`unspecified` record for generic callers; managed owner paths must provide an
explicit kind. A non-zero `settled_volume` is valid only for `open` or `close`.
The durable store also rejects a compare-and-commit that changes the kind of an
existing record, so immutability is enforced below the in-memory journal API.

Readers continue to accept versions 1, 2, and 3. Version-3 records that contain
managed requested-volume metadata are migrated as `open`, because that version
was emitted only by the managed OPEN owner path. This preserves both unresolved
reconciliation and already-settled OPEN records across the upgrade. Earlier
records remain `unspecified` and are never treated as reconstructable OPEN
exposure by the managed owner.

`ManagedTradeOwner::recover_settled_open()` accepts only durable `open` records.
Close/cancel settlement and reconstruction are intentionally not implemented in
this slice; they will use the discriminator instead of inferring semantics from
the lifecycle state or settled volume alone.

## Consequences

- A future close fill cannot be replayed as an OPEN fill after restart.
- Legacy version-3 OPEN settlement remains recoverable without rewriting old
  files.
- Generic journal users retain the existing create API, but `unspecified`
  records cannot participate in managed semantic settlement.
- The next close/cancel slice can add provenance-bearing exit settlement without
  changing the identity of an operation or guessing its kind from payloads.

## Verification

The native journal tests validate unknown-kind rejection, explicit OPEN/CLOSE
round-trip persistence, and version-3 OPEN migration. Managed-owner tests
reject a durable CLOSE record in the OPEN recovery path. The existing formal
managed-trade model already names `open`, `close`, and `cancel` as distinct
operation kinds; this slice aligns the durable C++ record with that model.

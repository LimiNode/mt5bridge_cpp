# ADR-0031: Durable managed ownership basis

## Status

Accepted as the first managed-allocation boundary. The operation-scoped basis
record and Windows store are implemented; broker-volume conversion, allocation
entries, and managed exposure mutation remain deferred.

## Context

ADR-0030 proves only the broker facts of an `INOUT` reversal. Its
`BrokerAllocationEnvelope` intentionally leaves both broker legs unallocated.
That envelope cannot answer which managed operation, if any, may consume part of
the broker close leg. Treating a caller-supplied volume as proof would recreate
the attribution bug that this boundary is meant to prevent.

The managed journal already retains a durable operation identity, immutable
operation kind, reconciliation descriptor, result-derived deal bindings,
revision, and authoritative managed `settled_volume`. Those facts can establish
an operation-scoped upper bound in the journal's logical volume domain, but they
do not establish a conversion to the broker's normalized symbol-step domain.

## Decision

Introduce `ManagedOwnershipBasis`, keyed by the composite:

```text
(BrokerReversalKey, OperationKey)
```

The composite identity is deliberate. One broker reversal may later be split
across multiple managed operations; a store keyed only by `BrokerReversalKey`
would make that future ledger impossible without changing its physical schema.

Each basis stores:

```text
account
broker reversal key
managed operation key
broker provenance copied from the envelope
source operation revision
max_managed_close_units
```

`max_managed_close_units` is exactly the durable operation's non-zero
`settled_volume`. It remains in the managed journal's logical unit domain. The
basis therefore says only:

```text
this settled close operation is associated with this proven broker deal,
and no later allocation may claim more than its durable logical settlement
without a newer basis
```

It does **not** say that those logical units equal any number of broker
`broker_close_leg` units. A later allocation slice must provide an explicit,
exact volume-domain conversion and per-deal settlement evidence before it may
create a broker allocation entry.

The proof-gated commit requires all of the following durable facts:

1. The operation record exists at the exact pinned revision and has the same
   account and `OperationKey`.
2. The operation is a durable `close` operation in `partially_filled` or
   `filled`, with a non-zero settled volume not exceeding its requested volume.
3. Its descriptor is durable and contains exactly one `history_deal_present`
   predicate resolved to the referenced `DEAL_TICKET` (directly or through one
   durable result binding).
4. The broker-only allocation envelope exists for the same reversal key and
   has matching account and provenance. Its two legs must still be completely
   unallocated.
5. The basis cap exactly equals the operation record's settled volume and the
   source operation revision exactly matches the basis revision.

Missing, stale, malformed, mismatched, or non-close source evidence fails
closed. A later operation revision does not silently update an old basis; it
requires a new composite-key record.

The basis contains no `managed_close_leg`, no `managed_reverse_open_leg`, and no
broker-volume cap. `broker_reverse_open_leg` is not allocatable from an `INOUT`
close basis. It remains unallocated until a separate managed-open intent and a
future proof establish why that opposite exposure belongs to a managed trade.

## Invariants

- Every basis is account-consistent and references valid broker and operation
  identities.
- A basis is accepted only after the broker envelope and operation record are
  already durable.
- The source operation revision and managed settled-volume cap are immutable.
- One broker reversal may have many operation-scoped basis records; one
  composite key has at most one immutable basis. Exact replay is idempotent and
  a different record for the same composite key conflicts.
- No basis commit changes `ManagedTradeState`, `OperationRecord`, or broker
  envelope state.
- No basis authorizes FIFO, LIFO, pro-rata, reverse-open attribution, or a
  broker-volume conversion.
- Restart scans are all-or-nothing and reject malformed or filename/key
  mismatches.

## Consequences

- The old one-broker-reversal/one-operation physical identity is not repeated.
- Ownership evidence is now explicitly downstream of both durable broker proof
  and durable managed operation proof.
- A future ledger can leave a broker close leg partly or wholly unallocated and
  can split it across several `TradeId` values without migrating this basis
  store.
- Managed `DEAL_ENTRY_INOUT` settlement remains fail-closed until the later
  volume-mapping and allocation-entry slices are proven.

## Verification boundary

The implementation regressions cover:

- proof-gated commit from matching operation and broker-envelope records;
- caller-supplied cap, stale revision, wrong deal, open operation, missing
  operation, and missing envelope rejection;
- two operation keys sharing one broker reversal without conflict;
- exact replay, checksum-backed persistence, restart load, and composite-key
  scan; and
- header self-containment without adding runtime or public side effects.


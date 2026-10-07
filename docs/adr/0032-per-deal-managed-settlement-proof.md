# ADR-0032: Per-deal managed settlement proof

## Status

Accepted as a design-only boundary after ADR-0031. The durable association
between a close operation and a broker reversal exists, but no per-deal logical
settlement record or runtime producer is introduced by this ADR.

## Context

`OperationRecord::settled_volume` is a cumulative frontier for one logical
operation. A close operation may settle through several broker deals:

```text
deal A = 2
deal B = 3
settled_volume = 5
```

The cumulative value proves that the operation has settled five logical units,
but it does not prove that either deal individually owns five units. ADR-0031
therefore records only the durable association between the operation and one
broker reversal.

The next quantitative fact must be narrower:

```text
(OperationKey, DEAL_TICKET) -> exactly N managed logical units
```

That fact must be durable before any later layer converts managed logical units
to broker step-normalized volume or consumes a broker allocation envelope.

## Decision

The future immutable per-deal fact is keyed by the composite:

```text
(OperationKey, DEAL_TICKET)
```

and must pin at least:

```text
account
operation key
DEAL_TICKET
source operation revision
managed logical units
settlement observation provenance
```

The per-deal fact is append-only. Its `source_operation_revision` and
`settlement_observation` pin the moment at which that deal's contribution was
proved; different deals from one operation may therefore have different
revisions and provenance:

```text
(Operation, A) -> 2 @ revision 8  / P8
(Operation, B) -> 3 @ revision 11 / P11
```

The proof producer must derive the logical units from a fresh,
provenance-bearing managed reconciliation result. A caller may not submit an
arbitrary number and have it accepted merely because the operation's
cumulative `settled_volume` is large enough.

The growing cumulative frontier is a separate durable fact:

```text
OperationSettlementFrontier {
    operation_key
    operation_revision
    settled_volume
    entries = { deal_ticket, source_operation_revision, managed_logical_units }
}
```

The frontier is pinned to one operation revision and references immutable
per-deal facts whose source revisions are less than or equal to that frontier
revision. It proves that the referenced entry set is complete for that
frontier and that the exact sum of their logical units equals
`settled_volume` at that revision. A later frontier may append a newly proven
deal without rewriting an earlier per-deal fact or its provenance.

For an operation settled by more than one deal, the producer must emit the
complete frontier proof in addition to the immutable per-deal facts. If the
decomposition is incomplete, duplicated, stale, or cannot distinguish one
deal's logical contribution, the operation remains `reconciling` and no new
frontier is committed.

The proof does not choose or imply any broker-volume mapping. In particular,
it does not claim that managed logical units equal `DEAL_VOLUME`,
`broker_close_leg`, or `broker_reverse_open_leg`. It also does not allocate a
`TradeId`, select FIFO/LIFO/pro-rata, consume an allocation envelope, or mutate
managed exposure.

## Required source evidence

An immutable per-deal fact may be committed only when all of these are durable
and mutually consistent:

1. The operation exists at the exact `source_operation_revision`, is a `close`
   operation, and its durable lifecycle permits the proven contribution.
2. The entry identifies a deal from authoritative observation; an arbitrary
   caller-selected ticket is insufficient. Its source revision and provenance
   are immutable once committed.
3. The observation belongs to the same account/operation and carries complete
   deal identity and provenance for the exact logical contribution.

A frontier proof may be committed only when all of these additional facts are
   durable:

1. The operation exists at the exact frontier `operation_revision`, is a
   `close` operation, and is `partially_filled` or `filled` with non-zero
   cumulative settlement.
2. Every referenced entry has a source revision less than or equal to the
   frontier revision, belongs to the same account/operation, and is immutable.
3. The frontier references every deal contributing to its cumulative value
   exactly once. The sum is exact in managed logical units and does not overfill
   the operation request.
4. A later frontier may add entries or advance the cumulative value, but may
   not rewrite an earlier per-deal fact or claim completeness without a new
   frontier proof.

Missing fields, ambiguous identity, an `INOUT` row without reversal semantics,
an incomplete deal set, or a conflicting replay fails closed.

## Explicit non-goals

- no conversion between managed logical units and broker volume;
- no update to `BrokerAllocationEnvelope` or either broker leg;
- no ownership allocation to a `TradeId`;
- no FIFO, LIFO, pro-rata, netting, or hedging policy;
- no public `TradeManager` API;
- no relaxation of the existing fail-closed `DEAL_ENTRY_INOUT` behavior.

## Consequences

- A cumulative journal frontier can no longer be reused as a per-deal cap.
- Multi-deal close settlement has an explicit place to record exact logical
  contributions before volume-domain mapping is considered.
- The next implementation slice must define the authoritative producer and
  durable replay format together; a value object with caller-supplied units is
  not sufficient proof.
- Until that implementation exists, managed `INOUT` settlement and broker-leg
  allocation remain unchanged and fail closed.

## Verification boundary

The eventual implementation must cover at least:

- one operation with two deals proven at different revisions, plus a frontier
  whose logical contributions sum exactly to its cumulative settled value;
- rejection of a frontier that references a missing per-deal fact or assigns
  the cumulative total to one deal;
- duplicate ticket, missing ticket, stale revision, incomplete decomposition,
  rewritten per-deal fact, and conflicting replay rejection;
- restart reconstruction from immutable per-deal records and frontier proofs;
- proof that no broker-volume or managed-exposure mutation occurs in this
  slice.

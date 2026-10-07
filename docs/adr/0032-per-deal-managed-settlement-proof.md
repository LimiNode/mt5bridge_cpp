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

The future per-deal proof is keyed by the composite:

```text
(OperationKey, DEAL_TICKET)
```

and must pin at least:

```text
account
operation key
DEAL_TICKET
operation revision
managed logical units
settlement observation provenance
```

The proof producer must derive the logical units from a fresh,
provenance-bearing managed reconciliation result. A caller may not submit an
arbitrary number and have it accepted merely because the operation's
cumulative `settled_volume` is large enough.

For an operation settled by more than one deal, the producer must emit a
complete per-deal decomposition for the settled frontier. The durable sum of
the entries must equal the operation's cumulative `settled_volume`, with each
deal ticket unique and each entry tied to the same operation revision and
observation proof. If the decomposition is incomplete, duplicated, stale, or
cannot distinguish one deal's logical contribution, the operation remains
`reconciling` and no per-deal proof is committed.

The proof does not choose or imply any broker-volume mapping. In particular,
it does not claim that managed logical units equal `DEAL_VOLUME`,
`broker_close_leg`, or `broker_reverse_open_leg`. It also does not allocate a
`TradeId`, select FIFO/LIFO/pro-rata, consume an allocation envelope, or mutate
managed exposure.

## Required source evidence

An implementation may commit a per-deal proof only when all of these are
durable and mutually consistent:

1. The operation exists at the exact pinned revision, is a `close` operation,
   and is `partially_filled` or `filled` with non-zero cumulative settlement.
2. The operation descriptor and result-derived bindings identify the target
   deal ticket exactly; an arbitrary caller-selected ticket is insufficient.
3. The settlement observation is newer than the operation baseline, belongs to
   the same graph/account, and carries complete deal identity and provenance.
4. Every deal contributing to the cumulative frontier has one unique durable
   `(OperationKey, DEAL_TICKET)` entry. The sum is exact in managed logical
   units and does not overfill the operation request.
5. The observation proof is immutable for the pinned operation revision, so a
   later snapshot cannot silently rewrite an earlier deal contribution.

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

- one operation with two deals whose logical contributions sum exactly to its
  cumulative settled frontier;
- rejection of a target deal assigned the cumulative total;
- duplicate ticket, missing ticket, stale revision, incomplete decomposition,
  and conflicting replay rejection;
- restart reconstruction from immutable per-deal records; and
- proof that no broker-volume or managed-exposure mutation occurs in this
  slice.

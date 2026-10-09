# ADR-0033: Exact managed logical-unit to broker-volume mapping

## Status

Accepted as a bounded conversion slice after ADR-0032. The slice defines an
exact decimal conversion fact but does not persist it, allocate broker legs, or
mutate managed exposure.

## Context

ADR-0032 durably proves an immutable managed quantity for one
`(OperationKey, DEAL_TICKET)`, while ADR-0030 durably proves broker-level
reversal legs in a symbol-step-normalized volume domain. The two quantities are
intentionally different domains. Treating their integer fields as equal would
silently turn a logical-unit fact into an ownership or broker-volume claim.

The conversion must therefore be exact, bounded, and independently replayable:

```text
managed_logical_units / 10^managed_scale
    == broker_volume.units / 10^broker_scale
```

and the resulting broker quantity must be aligned to the observed symbol
volume step.

## Decision

The conversion boundary uses two explicit pieces of evidence:

1. `ManagedLogicalVolumeNormalization::scale` identifies the decimal domain
   of the managed quantity. It must come from the authoritative operation or
   symbol contract; a caller may not invent a scale to force a candidate
   mapping into a desired result.
2. `BrokerVolumeNormalization` is taken from the durable broker-observation
   proof itself. A caller cannot provide a second scale or step that would
   reinterpret the broker record.

Conversion is performed with bounded integer arithmetic only. When the broker
scale is finer, the managed numerator is multiplied by an exact power of ten;
when it is coarser, division is accepted only when there is no remainder. The
result must be positive and divisible by `step_units`; otherwise the mapping is
not representable and fails closed.

`ManagedBrokerVolumeMapping` binds the conversion to an immutable managed
per-deal fact and a durable broker deal identity/provenance. It records the
converted broker quantity, but it deliberately does not compare that quantity
with `broker_close_leg` or `broker_reverse_open_leg`. A converted quantity is
not yet an ownership allocation and cannot consume either unallocated leg.

The current producer consumes `BrokerReversalRecord` because that is the only
durable broker deal proof available at this boundary. A non-reversal deal is
not treated as mapping evidence merely because a raw history row contains a
volume; an equivalent durable proof must be introduced before that case is
admitted.

The current slice exposes pure derivation only. No mapping store, recovery
manifest, owner-loop call, or public `TradeManager` is introduced. A later
allocation slice must obtain authoritative scale evidence, validate the mapping
against its durable sources, and decide whether/how a mapped quantity can be
allocated to a broker leg.

## Required invariants

- managed and broker identities refer to the same account and deal ticket;
- both source provenance records are complete and immutable;
- both decimal scales are bounded (`<= 9`);
- conversion uses no floating-point arithmetic;
- coarsening the broker scale is accepted only for an exact integer quotient;
- converted units are positive and exactly step-normalized;
- overflow, missing scale/step, non-integral conversion, and identity mismatch
  fail closed;
- the mapping does not imply FIFO/LIFO/pro-rata ownership or consume a broker
  leg;
- the same input proofs deterministically produce the same mapping.

## Explicit non-goals

- no durable mapping store or restart protocol;
- no allocation ledger, `TradeId` policy, FIFO/LIFO/pro-rata decision, or
  broker-leg consumption;
- no managed exposure mutation or public `TradeManager` API;
- no reinterpretation of `DEAL_VOLUME` as managed logical units;
- no conversion from arbitrary caller-supplied floating-point values.

## Verification boundary

Focused regressions cover equal scales, finer and coarser broker scales,
step-aligned and non-step-aligned values, non-integral conversion,
zero/invalid evidence, overflow, identity mismatch, and the fact that mapping
does not alter source proofs or broker allocation legs.

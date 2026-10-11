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
managed_logical_units / 10^0
    == broker_volume.units / 10^broker_scale
```

and the resulting broker quantity must be aligned to the observed symbol
volume step.

The integer representation is not, by itself, a physical unit contract. The
current managed settlement path supplies that missing contract: ADR-0026 only
accepts an attributed broker total when it is a positive whole-lot quantity
(`floor(total) == total`), and the resulting managed settlement fact stores
that authoritative integer quantity. This bounded slice therefore treats one
managed unit as one whole broker-volume unit at scale zero. A future managed
quantity with a different physical unit or fractional lot contract must add
durable source evidence before it can use this mapping.

## Decision

The conversion boundary uses one managed-domain contract and one explicit
piece of broker evidence:

1. Managed quantities are integer units (`scale = 0`) under ADR-0024. The
   mapping API therefore does not accept a caller-supplied logical scale. A
   future non-integer managed quantity contract must add durable scale
   evidence before it can be admitted; it cannot reinterpret this field.
2. `BrokerVolumeNormalization` is taken from the durable broker-observation
   proof itself. A caller cannot provide a second scale or step that would
   reinterpret the broker record.

Conversion is performed with bounded integer arithmetic only. When the broker
scale is non-zero, the integer managed numerator is multiplied by an exact
power of ten. The result must be positive and divisible by `step_units`; otherwise
the mapping is not representable and fails closed.

`ManagedBrokerVolumeMapping` binds the conversion to an immutable managed
per-deal fact and a durable broker deal identity/provenance. It records the
converted broker quantity, but it deliberately does not compare that quantity
with `broker_close_leg` or `broker_reverse_open_leg`. It does, however, require
the converted quantity to be no greater than the proven `deal_volume` in the
same exact scale/step domain. This is a consistency bound on one deal, not an
allocation decision; the mapping cannot consume either unallocated leg.

The current producer consumes `BrokerReversalRecord` because that is the only
durable broker deal proof available at this boundary. A non-reversal deal is
not treated as mapping evidence merely because a raw history row contains a
volume; an equivalent durable proof must be introduced before that case is
admitted.

The current slice exposes pure derivation only. No mapping store, recovery
manifest, owner-loop call, or public `TradeManager` is introduced. A future
non-integer managed-volume contract must first add durable scale evidence; the
allocation layer may then validate that new proof and decide whether/how a
mapped quantity can be allocated to a broker leg.

## Required invariants

- managed and broker identities refer to the same account and deal ticket;
- both source provenance records are complete and immutable;
- the broker decimal scale is bounded (`<= 9`); the managed scale is the
  durable integer-domain contract (`0`);
- conversion uses no floating-point arithmetic;
- converted units are positive and exactly step-normalized;
- a derived mapping uses the same broker scale/step as `deal_volume` and does
  not exceed that deal volume;
- overflow, missing broker scale/step, and identity mismatch
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

Focused regressions cover the integer whole-lot domain, finer broker scales,
step-aligned and non-step-aligned values, invalid broker normalization,
overflow, identity mismatch, rejection of a mapping larger than `DEAL_VOLUME`,
and the fact that mapping does not alter source proofs or broker allocation
legs.

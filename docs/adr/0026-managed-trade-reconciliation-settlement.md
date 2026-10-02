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
- a post-dispatch graph revision newer than the durable descriptor baseline.

The current bounded implementation supports only an `OPEN` slice with a full
fill or deterministic rejection. A confirmed full fill uses the slice's
durable requested volume; it never accepts a caller-supplied volume. An
authoritatively ambiguous cycle may record `unknown` only when the same
provenance checks pass. Pending, not-observed, and event-gap cycles leave the
managed slice unresolved. Account mismatch, missing provenance, and durable
identity mismatch cannot settle the managed state.

Partial fills, remainder handling, close/cancel settlement, late fills, and
restart reconstruction of aggregate exposure remain later bounded slices. The
worker and owner therefore keep the durable journal non-resendable while those
semantics are absent.

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

`tests/managed_trade_owner_test.cpp` covers a full open fill through a real
`OperationReconciliationWorker` and verifies that the managed volume changes
only after the worker observes the required order. It also covers a pending
cycle: the journal is normalized for reconciliation while managed state stays
`submitting` and no second backend call is possible.

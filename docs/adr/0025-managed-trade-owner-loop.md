# ADR-0025: Private managed-trade owner loop

## Status

Accepted as the first composition slice between the private managed-trade state
machine and the durable dispatch/reconciliation infrastructure. It is private
C++ infrastructure, not a public `TradeManager` API.

## Decision

`dispatch::ManagedTradeOwner` owns the bounded owner-loop sequence for one open
slice:

```text
ManagedTradeState
  -> durable journal intent + descriptor
  -> DispatchAdmissionBarrier
  -> move-only DispatchPermit
  -> OneShotDispatchBackend
  -> durable raw result
  -> provenance-bearing reconciliation worker
  -> provenance-bearing lifecycle settlement
```

The owner first prepares the logical open slice on a candidate copy, then
persists `created -> prechecking -> prechecked ->
dispatch_intent_persisted` and the immutable reconciliation descriptor. The
candidate becomes owner state only after those durable mutations succeed.
`execute_pending()` cannot call a transport directly: it must obtain a fresh
`DispatchPermit`, and only `OneShotDispatchBackend` consumes that permit.

The backend's raw result remains opaque and durable in the journal. An accepted
or reconciling result leaves the managed slice unresolved and returns
`awaiting_reconciliation`; this seam deliberately has no plain caller-supplied
evidence API. Only `settle_reconciliation()` with an
`OperationReconciliationWorker` cycle carrying provenance-bearing observation
may settle the managed state and journal. A deterministic broker rejection may
still settle immediately because
the backend has proved that no execution effect occurred. A transport failure,
a result that cannot be durably bound, or a lease/account failure after the
submitting edge is converted to durable `reconciling` with a non-resendable
ambiguous owner outcome; the operation remains `submitting` for recovery and
is never retried by this seam.

This first slice prepares and executes `OPEN` operations. Close/cancel planning,
restart reconstruction of aggregate managed exposure, and higher-level policy
remain subsequent slices.

## Consequences

- Managed state by itself is not a broker-send capability.
- Every attempted side effect is preceded by the durable dispatch barrier and
  an account/fencing/proof check in the same owner loop.
- An accepted result can be durable without being treated as a final fill;
  reconciliation supplies authoritative, provenance-bearing evidence later.
- Uncertain execution remains non-resendable across the owner loop.
- The public SDK, C ABI, Python runtime, and public `TradeManager` remain
  unchanged.

## Verification

`tests/managed_trade_owner_test.cpp` checks that an admission rejection never
reaches the transport, an accepted result cannot be settled by an arbitrary
caller-supplied fill, a full open fill is settled only through a real
provenance-bearing worker cycle, and both transport uncertainty and a
post-barrier pre-transport account failure produce durable
`reconciling`/`reconcile_only` state with no second backend call.

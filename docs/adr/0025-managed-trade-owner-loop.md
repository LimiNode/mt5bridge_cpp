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
  -> logical broker evidence
  -> ManagedTradeState + journal settlement
```

The owner first prepares the logical open slice on a candidate copy, then
persists `created -> prechecking -> prechecked ->
dispatch_intent_persisted` and the immutable reconciliation descriptor. The
candidate becomes owner state only after those durable mutations succeed.
`execute_pending()` cannot call a transport directly: it must obtain a fresh
`DispatchPermit`, and only `OneShotDispatchBackend` consumes that permit.

The backend's raw result remains opaque and durable in the journal. The owner
accepts only logical evidence (`fill`, `cancel_accepted`, `rejected`, or
`unknown`) from the reconciliation layer. A durable result waits for that
evidence before the managed state is settled. A transport failure, a result
that cannot be durably bound, or a lease/account failure after the submitting
edge is converted to durable `reconciling + ambiguous`; it is never retried.

This first slice prepares and executes `OPEN` operations. Close/cancel planning,
restart reconstruction of aggregate managed exposure, and higher-level policy
remain subsequent slices.

## Consequences

- Managed state by itself is not a broker-send capability.
- Every attempted side effect is preceded by the durable dispatch barrier and
  an account/fencing/proof check in the same owner loop.
- A result can be durable without being treated as a final fill; reconciliation
  supplies the logical volume evidence separately.
- Uncertain execution remains non-resendable across the owner loop.
- The public SDK, C ABI, Python runtime, and public `TradeManager` remain
  unchanged.

## Verification

`tests/managed_trade_owner_test.cpp` checks that an admission rejection never
reaches the transport, an admitted result requires explicit logical fill
evidence before settling the state, and a transport failure produces durable
`ambiguous` state with no second backend call.

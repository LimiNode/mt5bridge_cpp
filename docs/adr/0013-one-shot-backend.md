# ADR-0013: Internal one-shot backend and broker rejection handling

## Status

Accepted. This slice defines the guarded internal execution seam after durable
admission. It does not expose an unmanaged C ABI send function.

## Context

The durable admission barrier returns a move-only `DispatchPermit`, but the
permit alone must not be treated as permission to call a backend. Ownership,
the journal revision, immutable account, and fencing token can become stale
between admission and execution. A backend result also has to be persisted
before the operation lifecycle claims an outcome.

MetaTrader's `order_check()` is advisory and does not prove that the current
trade session is open. In particular, a broker can return
`MqlTradeResult.retcode == 10018` (`TRADE_RETCODE_MARKET_CLOSED`) from
`order_send()` after `order_check()` returned `Done`. This is a deterministic
broker rejection, not a transport failure or an ambiguous outcome.

## Decision

`runtime::OneShotDispatchBackend` is a private, Python-free execution guard.
It consumes a moved `DispatchPermit` and an injected `DispatchTransport`; a
terminal-specific adapter remains behind that private interface. Immediately
before the call it verifies:

1. the permit is valid and belongs to the requested `OperationKey`;
2. the owner-loop record is still `dispatching + prechecking`, with the exact
   permit revision and fencing token;
3. the current `AccountKey` matches the immutable operation account;
4. the single-writer lease still returns the permit's fencing token.

The journal then durably advances to `submitting`. The lease and account are
checked once more immediately before the transport call. The transport is
called exactly once and is never retried by this layer.

The result contract is:

```text
dispatching + submitting
        ↓ one backend call
persist_result(full raw MqlTradeResult)
        ↓
retcode 10018 → OperationState::rejected
```

`OperationState::rejected` is allowed from `submitting` only after a non-empty
result payload is durable. The corresponding valid journal pair is
`result_persisted + rejected`. A transport failure or an exception leaves the
operation at `dispatching + submitting` with no result; recovery must mark it
ambiguous/reconciling and must never resend it. Failure to persist a returned
result is likewise fail-closed.

Other broker results carry an explicit adapter disposition: accepted results
enter `accepted`, deterministic rejections enter `rejected`, and results that
need identity evidence enter `reconciling`. The raw payload is preserved in
all three cases.

The implementation intentionally does not infer session-open state from
`trade_mode`, tick age, weekday, or `order_check`. A future session schedule
query may be advisory only; broker retcodes remain authoritative.

## Consequences

- A stale permit cannot reach the transport call.
- Losing the lease after `submitting` but before the call stops execution
  without issuing a side effect; the operation remains non-resendable.
- `10018 MARKET_CLOSED` becomes a durable terminal rejection with no retry and
  no unnecessary reconciliation cycle.
- Python exceptions and transport failures cannot be mistaken for broker
  decisions.
- The public C ABI and C++ consumer facade still expose no unmanaged
  `order_send` method.

## Verification

`tests/one_shot_backend_test.cpp` covers a real scope-bound permit followed by
`10018`, durable raw-result persistence, final `rejected` state, permit
consumption/no retry, lease loss before the call, and transport failure leaving
the operation unresolved without a second call. The test uses a fake transport
and never touches Python or MT5.

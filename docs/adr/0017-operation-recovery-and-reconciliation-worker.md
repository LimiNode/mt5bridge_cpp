# ADR-0017: Operation recovery and journal-aware reconciliation worker

## Status

Accepted. This slice adds owner-loop recovery classification and a bounded
reconciliation wrapper; it does not add a thread, retry, or a public
`order_send` API.

## Context

`OperationJournal::recover_all()` restores durable records after a process
restart, while `ReconciliationWorker` already performs one authoritative
observation cycle. The missing boundary was the owner-loop policy that decides
whether a recovered record may continue pre-dispatch validation or has crossed
the non-resendable dispatch barrier. A crash after durable `dispatching` must
never turn into a second send.

## Decision

`OperationRecoveryCoordinator::recover()` calls `recover_all()` and classifies
the complete result set atomically:

```text
created/prechecked, pre-dispatch lifecycle
    -> resume_pre_dispatch

dispatching, result_persisted, reconciling
    -> reconcile_only

filled/cancelled/expired/rejected/failed/ambiguous
    -> terminal
```

Terminal lifecycle states are classified before journal state so a persisted
broker rejection is not reopened merely because its journal contains a result.
Any record at or beyond `dispatching` is non-resendable.

`OperationReconciliationWorker` binds one recovered `OperationKey` to the
caller-driven observation worker. Before the first refresh it normalizes a
post-dispatch record to `reconciling` durably. It accepts explicit predicates
and an explicit lifecycle state that those predicates prove; an opaque broker
result remains a hint and is never used to manufacture identity. The durable
predicate/baseline contract is retained in the operation record as specified by
[ADR-0020](0020-durable-reconciliation-descriptor.md); a worker reconstructed
against a new graph instance re-anchors only its in-memory baseline before the
first refresh.

The mapping is deliberately fail-closed:

```text
confirmed       -> durable caller-selected terminal/partial state
ambiguous       -> durable ambiguous
pending         -> leave reconciling
not_observed    -> leave reconciling
trade_event_gap -> leave reconciling
account_mismatch-> leave reconciling and suspend this worker; the owner creates
                  a new worker only after the correct account is restored
```

Only the `confirmed` and contradictory-evidence paths mutate the lifecycle.
No path creates a new `DispatchPermit` or invokes a transport. A partial fill
is represented by `OperationState::partially_filled`; a later remainder must
be handled by a new caller-owned worker and a new operation identity after the
remainder is proven.

## Consequences

- Restart discovery becomes an explicit, testable no-resend policy.
- Durable `submitting` and `accepted` records resume through observation only.
- Deadlines and missing visibility do not become failure or permission to retry.
- The public C ABI and runtime transport remain unchanged.
- A higher-level `TradeManager` still owns intent creation, scheduling,
  callbacks, and close obligations.

## Verification

`tests/operation_reconciliation_worker_test.cpp` covers discovery without a
known key, normalization of a recovered dispatching record, pending evidence,
event gaps, account mismatch suspension, contradictory evidence settling to
durable `ambiguous`, explicit `partially_filled` settlement, durable
confirmation, and terminal classification after restart. Existing journal,
one-shot backend, observation, and header self-containment tests are unchanged
and remain part of the focused runtime-off suite.

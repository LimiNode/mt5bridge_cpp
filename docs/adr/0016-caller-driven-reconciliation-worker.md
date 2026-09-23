# ADR-0016: Caller-driven reconciliation worker

## Status

Accepted. This slice adds the observation/TradeManager seam only; it does not
start a background thread or invoke `order_send`.

## Context

The observation coordinator and reconciliation engine already separate
authoritative refresh from pure predicate evaluation. A managed trade needs an
owner-loop component that runs those two actions together while retaining the
accepted graph provenance. It must also keep `pending` and `not_observed`
operations unresolved after a deadline so a later snapshot or restart can
continue reconciliation without enabling a resend.

## Decision

`mt5bridge::ReconciliationWorker` is a caller-driven, one-cycle-at-a-time
wrapper around `ObservationCoordinator`:

```text
owner loop calls step()
    -> coordinator.refresh(collection_request)
    -> coordinator.evaluate(reconciliation_request + hints)
    -> return refresh sample + reconciliation result
```

The worker stores the collection request, operation baseline, and predicates.
Each `step()` accepts explicit `trade_event_gap` and `deadline_expired` hints.
The cycle result contains the refresh admission/sample and the evaluated
result. Once a cycle is settled (`confirmed`, `account_mismatch`, or
`ambiguous`), later calls return that cycle without another provider refresh.
Unresolved `pending`/`not_observed` cycles remain eligible for a later owner
call. No thread, timer, transport retry, or side effect is hidden in this
class.

Startup recovery remains explicit: the owner first calls
`OperationJournal::recover_all()`, then creates operation-specific workers from
the recovered records and immutable keys. The worker never guesses predicates
from an opaque broker result or silently converts a deadline into a terminal
trade state.

## Consequences

- TradeManager can own scheduling and callback delivery without coupling the
  observation graph to a runtime thread.
- Accepted sample provenance and evaluated revision are returned together,
  preventing decisions from being attributed to a later graph mutation.
- `not_observed` does not end operation lifetime or permit automatic resend.
- The public C ABI remains unchanged and no unmanaged `order_send` is exposed.

## Verification

`tests/reconciliation_worker_test.cpp` verifies one accepted refresh/evaluation
cycle, cycle provenance retention, and that a settled worker does not perform
an extra provider refresh. Existing graph, environment, journal, backend, and
runtime tests remain unchanged.

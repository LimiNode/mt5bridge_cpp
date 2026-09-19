# ADR-0010: Bounded cross-view environment consistency

## Status

Accepted. This slice adds a pure observation policy before the durable journal
and before any side effect. It does not call MT5, start a worker, or submit an
order.

## Context

`DispatchConsistencyGate` proves that the requested graph domains are fresh
relative to a baseline. It cannot prove that sequential `orders()`,
`positions()`, and history calls observed one atomic MetaTrader state. The
terminal can publish a deal before its history order, update a position while
another view is being read, or change accounts between calls. An
`account_before`/`account_after` sandwich catches only changes that remain
visible at the two endpoints.

The next layer therefore needs a bounded, evidence-based question:

> Do repeated observations remain account-scoped and agree on the identity
> links required by the next pipeline stage?

It must not answer the stronger and generally unobservable question of whether
the broker supplied a single atomic cross-domain snapshot.

## Decision

`EnvironmentConsistencyPolicy` in
`include/mt5bridge/environment_consistency.hpp` evaluates a caller-owned
sequence of `ObservationBatch` values. The caller chooses the exact domains
and history windows with `EnvironmentConsistencyRequest`; at least two and at
most eight sequential batches may be supplied for one bounded cycle.

Every batch must satisfy all of the following before it can contribute
evidence:

- the account key is complete and equal to every other batch's `(server,
  login)` identity;
- observed domains, history windows, and payload namespaces exactly match the
  request;
- graph-required known fields and unique primary tickets are present;
- history records retain native millisecond timestamps inside their requested
  windows.

The policy then checks the links that can be proved from the supplied views:

- active orders with a non-zero `ORDER_POSITION_ID` or known non-zero
  `ORDER_POSITION_BY_ID` must have a currently observed matching
  `POSITION_IDENTIFIER` when the positions domain is requested;
- an observed deal whose `DEAL_ORDER` is not yet present in the requested order
  view is provisional, not contradictory, because MT5 may publish the deal
  first;
- when the same order is present in active/history orders and deals, non-zero
  position identifiers must agree;
- duplicate position identifiers attached to different current position
  tickets are contradictory;
- a deal may refer to a position that is no longer active: current-position
  absence alone is not a contradiction for a historical lifecycle.

The result has explicit fail-closed states:

```text
consistent
awaiting_confirmation
cross_view_mismatch
account_changed
unstable_environment
insufficient_evidence
invalid_request
```

`consistent` requires two consecutive batches with no unresolved links and the
same deterministic identity/link signature. A missing counterpart or a deal
that precedes its history order yields `awaiting_confirmation`. Directly
conflicting links yield `cross_view_mismatch`; an account switch yields
`account_changed`; coherent signatures that keep changing until the requested
bounded budget is exhausted yield `unstable_environment`. Missing required
domains, malformed records, and incomplete known fields yield
`insufficient_evidence`; a malformed policy scope yields `invalid_request`.

The policy is deliberately separate from `DispatchConsistencyGate`:

```text
ObservationCoordinator
        |
        +--> DispatchConsistencyGate       freshness/provenance
        |
        +--> EnvironmentConsistencyPolicy  bounded cross-view agreement
        |
        +--> future journal barrier        durable side-effect admission
```

Neither `ready` from the gate nor `consistent` from this policy authorizes
`order_send` by itself. The eventual journal barrier must still verify the
operation account, capabilities, writer ownership, and durable dispatch state.

## Consequences

- Delayed MT5 publication is represented as provisional evidence instead of a
  false contradiction.
- Identity-link disagreements are detected before a future dispatch layer can
  mistake incompatible views for one state.
- The bounded retry budget makes instability explicit and prevents an implicit
  infinite polling loop.
- The policy remains deterministic, header-only, and independent of Python,
  MT5 runtime, journal state, and side effects.
- A short-lived account sandwich or endpoint-stable epoch change remains
  outside the policy's proof. Such limits are documented rather than hidden.

## Verification

`tests/environment_consistency_test.cpp` covers stable repeated views,
single-observation waiting, missing active-position links, delayed
deal-before-order publication, direct order/deal mismatch, account changes,
bounded instability, closed-position history, malformed known fields, and
invalid empty requests. The test uses only deterministic POD batches and does
not load Python or MetaTrader5.

# ADR-0009: Observation coordinator and pre-dispatch consistency gate

## Status

Accepted. This slice coordinates synchronous authoritative observations and
checks pre-dispatch consistency. It does not submit orders or persist state.

## Context

`ObservationGraph` and `ReconciliationEngine` deliberately have no runtime
knowledge. A caller still needs one owner path that assembles account-wide
snapshots, applies them atomically, and asks whether a future dispatch layer
has enough fresh evidence. Calling the typed `Client` directly from several
callers would make it easy to mark filtered queries as authoritative or to
evaluate a baseline against a different graph instance.

## Decision

`ObservationProvider` is the testable seam for one complete collection. Its
`collect()` operation returns an `ObservationBatch`; the coordinator applies
that batch only after the provider has assembled exactly the requested domains,
windows, and payload namespaces. Extra or missing domains are rejected before
the graph apply, so an accidental history response cannot create unrelated
freshness evidence. The provided `ClientObservationProvider` uses unfiltered
`Client::orders()` and `Client::positions()` for authoritative active snapshots
and bounded millisecond windows for history. It validates the account identity
before querying and again after all queries; a change or unknown identity
discards the whole batch. A provider exception or shape violation leaves the
graph unchanged because the apply step has not started.

`ObservationCoordinator` owns one non-copyable `ObservationGraph` and one
provider reference. `refresh()` is synchronous and caller-driven: it does not
create a worker thread, retry a failed call, or infer a deadline. After a
successful graph apply it returns an `ObservationRefreshResult` containing an
`ObservationSample` whose constructor is private to the coordinator. The
sample carries the accepted batch, graph instance identity, and exact graph
revision, so a later policy cannot treat a copied batch as a new observation or
silently combine samples from another graph. The caller captures a
`ReconciliationBaseline` after an initial observation and retains it for
subsequent checks on that same coordinator graph.

`DispatchConsistencyGate` is a pure observation-readiness predicate, not a
proof of an atomic MT5 environment snapshot. Its sequential provider calls may
observe different terminal epochs; the gate verifies:

- a valid optional baseline and monotonic graph revision;
- account and process-local graph provenance;
- absence of an explicit event gap or unresolved prior operation;
- post-baseline active/position domain revisions when requested;
- complete post-baseline history coverage for requested windows.

It returns `ready` only when those evidence requirements hold. Other states
(`waiting_for_active_orders`, `waiting_for_positions`, `waiting_for_history`,
`unresolved_operation`, `account_mismatch`, `graph_mismatch`, `event_gap`, or
`invalid_request`) are non-authorizing. `ready` is an observation result, not a
permission to call `order_send`. A request with no active, position, or history
requirement is invalid rather than vacuously ready. The bounded
`EnvironmentConsistencyPolicy` in ADR-0010 adds stronger cross-view rules
without changing this observation layer.

## Consequences

- Snapshot assembly has one explicit owner path and a fake-provider test seam.
- Active queries remain account-wide; filtered active queries cannot be passed
  accidentally as authoritative coordinator evidence.
- Graph application stays atomic and separate from runtime calls.
- Gate decisions are deterministic and explain why more evidence is required.
- The gate cannot be mistaken for an atomic cross-domain MT5 snapshot proof.
- No journal, WAL, worker thread, implicit retry, `order_send`, or other trade
  side effect is introduced by this stage.

## Verification

`tests/reconciliation_coordinator_test.cpp` covers empty-request rejection,
baseline freshness, exact request/batch validation with graph immutability,
authoritative active/position refresh, bounded history coverage, event gaps,
unresolved operations, vacuous-gate rejection, account mismatch, graph
provenance mismatch, and accepted-sample production. The test uses only a
deterministic fake provider; it does not load Python or MetaTrader5. The
before/after account guard in `ClientObservationProvider` is enforced before
graph commit and remains independent of graph commit atomicity.

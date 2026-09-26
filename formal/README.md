# Formal state-machine checks

This directory contains deliberately small TLA+ models for the state machines
whose correctness depends on crashes, fencing, retries, and eventual
observation. The models use finite symbolic identities; they do not model
Python serialization, Win32 file I/O, C ABI layout, or market-data payload
conversion.

The first model is [dispatch/dispatch_recovery.tla](dispatch/dispatch_recovery.tla).
It covers the current durable pre-side-effect contract:

- one non-resendable `dispatching` barrier per operation;
- monotonically increasing lease epochs and current-token checks;
- a one-shot permit that is consumed at the transport boundary;
- crash/restart recovery to reconciliation rather than resend;
- at-most-one broker send;
- atomic result and reconciliation-evidence persistence;
- terminal resolution only after durable evidence.

Run it with the pinned TLA+ Tools v1.8.0 artifact (or a compatible TLC
distribution):

```text
pushd formal/dispatch
java -cp ../../tla2tools.jar tlc2.TLC -config dispatch_recovery.cfg \
    dispatch_recovery.tla
popd
```

The finite configuration uses two writers, one operation, and two lease
epochs. The model treats a broker result as volatile until the same writer
persists both the result and result-derived bindings. A crash clears that
volatile result, releases the process lease, and leaves the operation in a
non-resendable state. A replacement writer may only observe/reconcile it; the
dispatch permit and its writer/epoch pair cannot be reused.

Every TLC counterexample should become a focused C++ regression test; the
corresponding durable `OperationState` transition should remain visible in the
model.

The repository CI runs this model with a pinned TLC distribution and verifies
the JAR checksum before execution. Local runs use the same command shown
above; Java 17 or newer is sufficient.

The second model is
[reconciliation/reconciliation_evidence.tla](reconciliation/reconciliation_evidence.tla).
It checks observation ordering independently from dispatch recovery:

- active orders, positions, history orders, and history deals refresh as
  independent domains, so a deal may arrive before its history order;
- a domain that has not advanced past the durable baseline remains stale, and
  absence without authoritative coverage is not evidence of non-execution;
- contradictory/non-unique evidence is ambiguous rather than confirmed;
- an event gap blocks unresolved missing/pending evidence, matching the C++
  engine's `trade_event_gap` policy;
- account and graph-instance changes cannot produce confirmation, including
  after a restart re-anchor; and
- restart re-anchors the baseline without manufacturing fresh evidence.

Run it with the same pinned TLC artifact:

```text
pushd formal/reconciliation
java -cp ../../tla2tools.jar tlc2.TLC -config reconciliation_evidence.cfg \
    reconciliation_evidence.tla
java -cp ../../tla2tools.jar tlc2.TLC \
    -config reconciliation_evidence_refresh.cfg \
    reconciliation_evidence.tla
popd
```

The main configuration explores all four domains and scoped requests. The
refresh configuration uses two domains with a revision bound of two, making
repeated refresh, post-gap, and post-restart paths explicit without making the
primary ordering run unbounded. History absence still requires authoritative
coverage, making the model explicit about the difference between a
stale/lagging view and proof of absence.

The third model is
[managed_trade/managed_trade_lifecycle.tla](managed_trade/managed_trade_lifecycle.tla).
It sits above one-shot dispatch and models the logical lifecycle decisions:

- a running plan creates bounded open slices, while stopping it does not erase
  already observed exposure or a pending remainder;
- partial fills preserve both filled volume and the active remainder, which can
  later be observed or explicitly cancelled;
- `CloseObligation` is durable desired state rather than another operation;
- a close is satisfied only after reconciled evidence reduces exposure to zero;
  and
- an unknown outcome is ambiguous and cannot be retired for a new decision
  without a fresh snapshot.

Run it with the same pinned TLC artifact:

```text
pushd formal/managed_trade
java -cp ../../tla2tools.jar tlc2.TLC \
    -config managed_trade_lifecycle.cfg \
    managed_trade_lifecycle.tla
popd
```

The model intentionally does not expose a public `TradeManager` or invoke a
terminal side effect. A counterexample becomes a focused native lifecycle
test before the asynchronous trading API is added.

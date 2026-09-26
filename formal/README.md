# Formal state-machine checks

This directory contains deliberately small TLA+ models for the state machines
whose correctness depends on crashes, fencing, retries, and eventual
observation. The models use finite symbolic identities; they do not model
Python serialization, Win32 file I/O, C ABI layout, or market-data payload
conversion.

The first model is [dispatch/DispatchRecovery.tla](dispatch/DispatchRecovery.tla).
It covers the current durable pre-side-effect contract:

- one non-resendable `dispatching` barrier per operation;
- monotonically increasing lease epochs and current-token checks;
- a one-shot permit that is consumed at the transport boundary;
- crash/restart recovery to reconciliation rather than resend;
- at-most-one broker send;
- atomic result and reconciliation-evidence persistence;
- terminal resolution only after durable evidence.

Run it with a TLA+ tools distribution (TLC 2.18 or newer):

```text
pushd formal/dispatch
java -cp ../../tla2tools.jar tlc2.TLC -config DispatchRecovery.cfg \
    DispatchRecovery.tla
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
[reconciliation/ReconciliationEvidence.tla](reconciliation/ReconciliationEvidence.tla).
It checks observation ordering independently from dispatch recovery:

- active orders, positions, history orders, and history deals refresh as
  independent domains, so a deal may arrive before its history order;
- a domain that has not advanced past the durable baseline remains stale, and
  absence without authoritative coverage is not evidence of non-execution;
- contradictory/non-unique evidence is ambiguous rather than confirmed;
- an event gap remains unresolved until every domain has a fresh authoritative
  snapshot;
- account and graph-instance changes cannot produce confirmation; and
- restart re-anchors the baseline without manufacturing fresh evidence.

Run it with the same pinned TLC artifact:

```text
pushd formal/reconciliation
java -cp ../../tla2tools.jar tlc2.TLC -config ReconciliationEvidence.cfg \
    ReconciliationEvidence.tla
popd
```

The finite configuration explores both an all-presence request and a history
order absence request. The latter requires authoritative coverage, making the
model explicit about the difference between a stale/lagging view and proof of
absence.

The next planned model is the managed trade lifecycle (`CloseObligation`,
partial fills, cancellation, and slicing).

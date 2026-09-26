# ADR-0021: Finite formal model for durable dispatch recovery

## Status

Accepted as a verification aid for the durable dispatch layer.

## Decision

Keep a small finite TLA+ model in `formal/dispatch/DispatchRecovery.tla` with
its checked configuration in `DispatchRecovery.cfg`. The model covers the
interleavings that are difficult to exhaustively review in C++:

- a durable `dispatching` barrier is never a resend path;
- a permit is tied to the writer and fencing epoch that opened the barrier;
- at most one broker send is attempted for an operation;
- a crash releases the process lease and discards an unpersisted result;
- result bytes and result-derived reconciliation bindings become durable in a
  single mutation; and
- terminal state requires durable result or independently observed broker
  evidence.

The model uses finite symbolic writers, operations, and fencing epochs. It does
not model Python objects, Win32 I/O, C ABI layout, or market-data payload
conversion; those remain the responsibility of unit and integration tests.
TLC 1.8.0 is downloaded by CI over HTTPS, verified by a pinned SHA-256, and
run with Java 17. The model is a safety net: a counterexample becomes a
focused native regression, and a green TLC run does not replace the native
test suite or a broker smoke test.

## Consequences

- Crash-before-send and may-have-been-sent paths are represented explicitly,
  instead of being inferred only from happy-path tests.
- The model makes the result/binding atomicity contract visible to reviewers.
- A deliberately finite state space keeps CI deterministic and fast; broader
  reconciliation and managed-trade lifecycle models remain separate follow-up
  slices.

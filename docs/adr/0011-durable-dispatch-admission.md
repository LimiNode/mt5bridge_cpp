# ADR-0011: Durable operation journal and dispatch admission barrier

## Status

Accepted. This slice defines and tests the pre-side-effect Stage 2 boundary;
it does not call or expose `order_send`.

## Context

Observation coordination and bounded environment consistency now provide
account-scoped evidence, but neither layer may authorize a side effect by
itself. A process can fail after an operation is accepted by the terminal and
before an in-memory result is recorded. Retrying from memory could therefore
duplicate a broker action. The dispatch boundary needs a durable intent,
immutable identity, and a writer-fencing decision before any backend call.

## Decision

`mt5bridge::OperationJournal` owns one serialized operation state machine over
a caller-provided `DurableJournalStore`. A store's `commit()` must return true
only after the complete record survives a process crash. The journal updates
its owner-loop cache only after that durable commit; a failed commit leaves the
previous cache and durable record unchanged.

The record contains:

- immutable `(server, login)` `AccountKey`;
- stable managed `TradeId` and one-attempt `OperationId`;
- the exact opaque request payload retained for recovery;
- separate `JournalState` and canonical `OperationState` values;
- a monotonic record revision and, from `dispatching` onward, a non-zero
  fencing token.

`JournalState` is deliberately separate from the lifecycle vocabulary:

```text
created
  -> prechecked
  -> dispatch_intent_persisted
  -> dispatching
  -> result_persisted
  -> reconciling
```

`OperationState` remains the public lifecycle vocabulary (`queued`,
`prechecking`, `submitting`, `accepted`, `reconciling`,
`partially_filled`, `filled`, `cancelled`, `expired`, `rejected`, `failed`,
`ambiguous`). Entering `submitting` is rejected until the journal is already
at `dispatching`, so the lifecycle cannot bypass the durable barrier.

`DispatchAdmissionBarrier` opens `dispatching` only when all of these checks
pass in the same owner loop:

1. the operation is present at `dispatch_intent_persisted`;
2. the freshly read current account and the environment result account match
   the operation's immutable `AccountKey`;
3. the environment result is `consistent`;
4. there is no unresolved prior operation and no event gap requiring a fresh
   authoritative observation;
5. a `SingleWriterLease` is continuously held for the account and exposes a
   non-zero fencing token.

The durable transition returns a `DispatchPermit` carrying the operation key,
fencing token, and committed journal revision. The current slice stops there:
it has no backend adapter and no `order_send`. A future internal backend must
consume the permit while retaining the same lease/fencing ownership; a
recovered `dispatching` record is never automatically resent.

## Consequences

- Crashes before `dispatch_intent_persisted` leave no side-effect intent.
- A failed storage commit cannot make an operation appear dispatched.
- Account switches, unresolved operations, event gaps, unstable environment
  evidence, and lease loss fail closed before the barrier.
- Recovery can distinguish a pre-side-effect intent from a non-resendable
  `dispatching` record without guessing from volatile memory.
- The persistence and lease implementations remain replaceable seams; this
  header-only slice does not prescribe a file format or OS primitive.

## Verification

`tests/dispatch_journal_test.cpp` covers invalid/duplicate intents, durable
revision sequencing, separate journal and operation states, blocked submitting
before the barrier, environment/account/blocker/lease rejection, failed
`dispatching` commits with no partial mutation, successful fencing admission,
second-admission rejection, and restart recovery that never reopens a
`dispatching` operation. The test uses a deterministic in-memory store as a
durability contract double and never loads Python, contacts MT5, or calls
`order_send`.

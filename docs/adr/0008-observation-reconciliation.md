# ADR-0008: Observation-only reconciliation engine

## Status

Accepted. This slice evaluates explicit evidence predicates over
`ObservationGraph`; it does not persist a journal or submit an order.

## Context

`ObservationGraph` stores account-scoped snapshots, but a global graph revision
alone cannot prove that a particular namespace was refreshed after an
operation baseline. History records are positive evidence and remain in the
graph after later queries, so a ticket that was seen before the baseline must
not be reused as new evidence.

## Decision

`ReconciliationEngine` is a pure C++ consumer-side evaluator. A caller captures
`ReconciliationBaseline` before an operation or observation cycle. The
baseline contains the account identity, process-local graph provenance, global
graph revision, and last revision for every observation domain. The baseline is
an immutable value created by `capture_reconciliation_baseline()`; its
provenance and revision fields are exposed read-only, so application code
cannot manufacture a coherent-looking baseline by editing individual counters.
`ReconciliationRequest` stores the baseline as `std::optional`; a missing
capture is explicit and rejected by the evaluator. A baseline from another
graph instance with the same account and revision counters is rejected as
`AMBIGUOUS` with `ReconciliationReason::graph_mismatch`. Graph instances are
non-copyable and non-movable, and the provenance is process-local rather than
durable; a process restart invalidates every old baseline.

The caller then supplies explicit `ReconciliationPredicate` values:

- active order present/absent;
- position present/absent;
- history order present/absent in an optional/required inclusive window;
- history deal present/absent in an optional/required inclusive window.

Active and position predicates require the corresponding domain revision to be
strictly greater than the baseline. History presence predicates require the
matching ticket's evidence revision to be newer than the baseline history
domain revision. History absence predicates require
`history_*_covered(window, since_revision)`; an old ticket retained in the
positive-evidence map does not count as post-baseline evidence. Any bounded
history predicate also requires the record's native millisecond time field to
be known; an observed ticket without that field is contradictory evidence, not
a guessed match. The evaluator validates predicates and the revision ordering
in the baseline before inspecting graph state.

`ReconciliationRequest::deadline_expired` is supplied by the caller that owns
the bounded wait. A fresh snapshot that does not yet satisfy a predicate is
still `PENDING`; the evaluator cannot infer that a deadline has elapsed from a
graph revision. Only when `deadline_expired` is true, all required evidence is
authoritative, and predicates remain unsatisfied does the result become
`NOT_OBSERVED`. An explicit event gap remains non-terminal until the required
snapshots arrive. `ReconciliationResult::resolved()` is true only for
`CONFIRMED`, `ACCOUNT_MISMATCH`, or `AMBIGUOUS`; `NOT_OBSERVED` remains an
unresolved observation that a later cycle may confirm or disambiguate.
The result counters distinguish predicates lacking fresh evidence
(`pending_predicates`), predicates currently unsatisfied despite fresh
evidence (`missing_predicates`), and unusable or conflicting evidence
(`contradictory_predicates`).

The evaluator returns one of the canonical outcomes:

```text
PENDING
CONFIRMED
NOT_OBSERVED
ACCOUNT_MISMATCH
TRADE_EVENT_GAP
AMBIGUOUS
```

`PENDING` means at least one required authoritative domain or history coverage
is not fresh, or a fresh snapshot has not yet satisfied a mutable predicate.
`CONFIRMED` means every predicate is satisfied. `NOT_OBSERVED` means the
caller-declared deadline elapsed with sufficient authoritative evidence but a
predicate remained unsatisfied; it does not prove that an operation did not
execute. Contradictory evidence, malformed predicates, or an invalid baseline
produce `AMBIGUOUS`. An explicitly reported incomplete event hint stream
produces `TRADE_EVENT_GAP` while required fresh snapshots are still missing.

The engine never calls Python/MT5, starts a worker thread, writes a journal,
or invokes `order_send`. A future coordinator may collect authoritative
snapshots and feed them to the graph, but application code must not treat
filtered queries as account-wide observations.

## Consequences

- Reconciliation cannot accidentally use stale active/position snapshots.
- Old history tickets cannot satisfy a new operation's positive predicate.
- Negative history evidence is accepted only with revision-aware complete
  coverage.
- Account mismatch and event gaps remain explicit outcomes instead of being
  collapsed into ordinary absence.
- Graph provenance is an in-memory guardrail, not a restart or persistence
  mechanism.
- Durable operation identity, deadlines, persistence, and side effects remain
  separate future stages.

## Verification

`tests/reconciliation_engine_test.cpp` covers baseline capture, fresh active
confirmation, stale-domain pending state, authoritative disappearance,
revision-aware history presence/absence, stale history ticket rejection,
deadline-gated `NOT_OBSERVED`, contradictory evidence, account mismatch,
same-account graph provenance mismatch, explicit missing baselines, invalid
predicates, and event-gap handling.

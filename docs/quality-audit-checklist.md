# Codebase quality audit checklist

This checklist is Gate 2 of the release roadmap. It is an audit aid, not an
authorization to perform a broad refactor in a feature PR. Findings should be
fixed in small, independently testable slices after the managed-trade scope is
stable.

## Findings register

Use one row per finding and link the regression or review evidence.

| ID | Severity | Area | Finding | Owner | Proposed slice | Regression/evidence | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Q-___ | P0/P1/P2/P3 | path or subsystem | concise invariant violation | named owner | bounded PR | test, tool, or review link | open/verified/deferred |

Severity is about correctness and recovery risk, not code aesthetics:

- **P0** — data loss, duplicate side effect, or unsafe authority boundary;
- **P1** — incorrect durable state or an unbounded fail-open path;
- **P2** — compatibility, diagnostics, or maintainability issue with a clear
  bounded impact;
- **P3** — cleanup that does not change supported behavior.

## Ownership and validation

- [ ] Every durable invariant has one named implementation owner.
- [ ] Repeated validation is factored into one helper per rule where the
      ownership boundary is stable.
- [ ] `valid()` methods check structural facts only; source authority is
      established by a separately reloadable evidence chain.
- [ ] Caller-supplied identity, revision, volume, or digest is never treated
      as proof merely because it is non-zero.
- [ ] Unsupported cases return an explicit diagnostic or fail closed.
- [ ] Operation, deal, and position identities are not collapsed into one
      ticket or one logical event.

## Durable state and recovery

- [ ] Every new durable record has a versioned canonical encoding.
- [ ] Atomic publication, temporary-file recovery, corruption handling, and
      bounded file/record sizes are tested with real files.
- [ ] Exact replay is idempotent; a conflicting payload is not overwritten.
- [ ] Restart scans are all-or-nothing and reload source evidence instead of
      reconstructing missing history from a current broker snapshot.
- [ ] Account-scoped fencing is held through every publication boundary.
- [ ] Journal CAS, allocation publication, and exposure mutation have explicit
      crash windows and recovery outcomes.
- [ ] Historical records remain valid under later journal revisions when the
      ADR says they are historical facts.

## Reconciliation and MT5 boundaries

- [ ] Read paths distinguish empty, partial, stale, unavailable, and complete
      observations.
- [ ] Inclusive cursors and overlap reconciliation handle equal timestamps,
      reordered rows, and history rewrites.
- [ ] Orders, deals, positions, and `POSITION_IDENTIFIER` are represented
      independently.
- [ ] A timeout after `order_send()` never causes a blind retry.
- [ ] `NOT_OBSERVED`, `AMBIGUOUS`, `GAP`, and `RESYNC_REQUIRED` remain distinct
      from confirmed absence.
- [ ] External/manual activity cannot be silently attributed to a managed
      operation.

## Concurrency, callbacks, and ABI

- [ ] No callback runs while the Python GIL, runtime mutex, or internal ring
      lock is held.
- [ ] Callback cancellation, consumer overflow, unsubscribe, shutdown, and
      restart have deterministic ownership and lifetime rules.
- [ ] Bounded queues expose backpressure and gaps rather than allocating
      without limit.
- [ ] Public ABI changes use explicit versions and fixed-width/plain C types.
- [ ] DLL-owned buffers have exactly one matching release operation.
- [ ] Private dispatch code does not add a second transport or public side
      effect API.

## Complexity and documentation

- [ ] Search for parallel pipelines before adding a helper or abstraction.
- [ ] Remove unreachable branches, dead interfaces, and comments that only
      repeat the next line.
- [ ] Large methods and classes have a real responsibility boundary before
      extraction is proposed.
- [ ] Source files and important private contracts have `\file`/`\brief`
      documentation plus lifetime, fencing, recovery, and side-effect notes.
- [ ] CMake targets and tests use the owning source directory and canonical
      include paths.
- [ ] Both focused and full Release tests run with assertions disabled where
      runtime checks matter.

## Required audit output

The completed audit publishes the findings register, a list of deferred
findings with rationale, the tests that cover each accepted invariant, and the
next bounded corrective PR. It does not mix wholesale formatting or speculative
abstractions into trade behavior changes.

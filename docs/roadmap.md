# mt5bridge_cpp release roadmap

This roadmap describes the order in which the bridge can become a dependable
building block for account observation and, later, copy-trading applications.
It is a planning document, not a promise of dates. Each gate requires a
separately reviewable change, reproducible tests, and an explicit residual-risk
record.

## Current baseline

`main` contains the durable journal, reconciliation, managed settlement and
allocation foundations, plus the C3 attribution prerequisite from ADR-0037.
Production `INOUT` attribution and managed exposure mutation remain
fail-closed. Draft PR #68 (C3A PR A) is intentionally separate from this
roadmap: it records the opening-provenance trust boundary but is not assumed
to be merged here.

The existing read path already exposes typed orders, positions, history orders,
history deals, observation graphs, reconciliation, and host-driven realtime
tick subscriptions. These are foundations for trade observation; they do not
yet constitute a durable trade-event stream.

## Release gates

### Gate 1 — Managed trade completion

Finish and stabilize the current owner/reconciliation contour:

- durable `OPEN`/`CLOSE`/`CANCEL` lifecycle and restart recovery;
- committed managed ownership and allocation provenance;
- the bounded exact-exclusive `INOUT` contract, with unsupported mixed and
  ambiguous netting cases remaining fail-closed;
- pending operations, partial fills, and remaining-volume obligations;
- account fencing and duplicate-dispatch protection; and
- explicit supported and unsupported scenarios.

The C3A sequence is split into independently reviewable slices: provenance
inventory, durable opening evidence, exact pre-position frontier reconstruction,
and integration preflight without publication. Owner wiring and exposure
mutation follow only after the evidence chain is real and restart-tested.

### Gate 2 — Codebase quality hardening

Audit the repository without changing trading semantics. Use
[`development-rules.md`](development-rules.md) and the project review rules.
The audit must identify one owner for each invariant and record findings with
severity, owner, affected path, and a regression test. It covers validation and
serialization duplication, parallel responsibility paths, dead interfaces,
over-large classes, C++/Python dispatch duplication, diagnostics, and Doxygen
coverage. The checklist is in
[`quality-audit-checklist.md`](quality-audit-checklist.md).

### Gate 3 — MT5 compatibility and reliability

Reconcile the implementation with the searchable registry in
[`mt5-quirks.md`](mt5-quirks.md) and the coverage matrix in
[`mt5-quirks-coverage.md`](mt5-quirks-coverage.md). For every applicable
quirk, record source evidence, current behavior, a deterministic regression,
live validation, and residual risk. Unknown behavior must produce diagnostics
or fail closed; it must not be converted into an assumption.

Required live acceptance includes cold history, reconnect, account switching,
delayed trade visibility, partial fills, and terminal restart on a demo
account. Fake-runtime tests remain useful for deterministic logic but do not
replace terminal validation.

### Gate 4 — Trade Event Observation

Add a separate observation contour for changes caused by manual trading, other
Expert Advisors, and the broker. The first implementation is polling-first:
active orders, positions, history orders, and history deals are reconciled into
an account-scoped event stream. An optional MQL5 transaction adapter may later
provide low-latency hints, but it is never the sole source of truth.

The event contract is defined by
[`ADR-0039`](adr/0039-trade-event-observation.md). It must distinguish orders,
deals, and positions; represent partial and repeated execution; preserve
identity and cursors across restart; expose gaps and unresolved evidence; and
deliver callbacks outside the GIL and runtime mutex. It must not claim
broker-level exactly-once delivery.

### Gate 5 — Application-level copy-trading commands

Only after observation is reliable should an application-specific copier be
validated. The bridge remains a library: it exposes observations and safe,
durable commands, while strategy, source/destination mapping, scaling, and
risk policy belong to the application.

The acceptance surface includes market and pending orders, modification and
cancellation, SL/TP changes, partial/full close, divergence detection, and
duplicate-execution protection. This gate is not a prerequisite for the first
stable observation release.

## Dependency sequence

| Milestone | Depends on | Delivers | Explicitly does not deliver |
| --- | --- | --- | --- |
| C3A provenance (PR #68 and follow-ups) | ADR-0037 and durable close foundations | Verified opening-evidence boundary and, later, durable ownership history | Generic mixed-owner netting, automatic legacy migration |
| Gate 1 completion | C3A evidence and existing owner recovery | Bounded managed-trade settlement with restart proofs | Event observation for foreign/manual activity |
| Quality hardening | Stable Gate 1 contracts | Findings register, simplified ownership, documented diagnostics | Feature expansion or cosmetic rewrites |
| MT5 reliability audit | Existing quirks registry and test harnesses | Coverage matrix and live acceptance evidence | Universal guarantees over terminal history |
| Trade Event Observation | Typed read APIs, observation graph, reconciliation, quality baseline | Polling-first external trade event stream with gaps and replay | Automatic copying or native Python push callbacks |
| Copy-trading application | Stable observation and safe command APIs | Application-owned orchestration and source/destination mapping | Exactly-once broker semantics |

No gate may silently consume an unmerged Draft PR. A subsequent implementation
PR must name its exact base and list the verification evidence for the gate it
advances.

## Verification and release evidence

Every implementation PR reports:

- exact base and head SHA;
- focused build/test results and full Release CTest where applicable;
- `git diff --check`;
- exact-head push and PR CI;
- crash/restart and fencing scenarios for durable changes;
- live-terminal evidence where a quirk cannot be simulated faithfully; and
- known blockers and residual risk.

Docs-only PRs report the changed contracts, source links, `git diff --check`,
and the next bounded implementation slice. Merge is a separate reviewed
action.

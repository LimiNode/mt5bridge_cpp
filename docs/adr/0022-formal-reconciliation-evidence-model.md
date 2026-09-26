# ADR-0022: Finite formal model for reconciliation evidence ordering

## Status

Accepted as a verification aid for observation-only reconciliation.

## Decision

Keep a small finite TLA+ model in
`formal/reconciliation/reconciliation_evidence.tla` with its checked
configuration in `reconciliation_evidence.cfg` and
`reconciliation_evidence_refresh.cfg`. The model deliberately keeps
the evidence domains independent:

- active orders, positions, history orders, and history deals may refresh in
  any order, including a deal before its history order;
- only a domain revision newer than the durable baseline is fresh evidence;
- an absence observation is sufficient for an absence predicate only when the
  view is authoritative, while missing visibility never proves non-execution;
- contradictory or non-unique evidence is ambiguous, never guessed as a
  confirmation;
- an event gap blocks unresolved missing/pending evidence, while an already
  satisfied scoped request follows the same confirmation rule as the C++
  engine;
- the operation account is immutable across restart re-anchor, so account and
  graph-instance mismatches cannot confirm a request; and
- restart re-anchors the in-memory baseline but produces no fresh evidence.

The main finite configuration explores all four evidence domains, including
an all-presence request, a scoped active-order request, and a history-order
absence request. A second compact configuration permits repeated refreshes of
two domains, covering post-gap and post-restart re-observation without making
the primary ordering run unbounded. TLC 1.8.0 is downloaded by CI over HTTPS,
verified by the same pinned SHA-256 as the dispatch model, and run with Java
17. The model does not replace focused C++ tests or a broker smoke test.

## Consequences

- The ordering assumptions behind `ReconciliationEngine` are executable and
  reviewable rather than implicit in examples.
- A lagging view, event gap, restart, account switch, or graph replacement
  has an explicit non-confirming state in the model.
- The model is intentionally separate from durable dispatch recovery; a
  counterexample should become a focused reconciliation regression without
  changing the C ABI or adding a side-effecting API.

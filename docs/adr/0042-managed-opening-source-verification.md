# ADR-0042: Authoritative managed-opening source verification

## Status

Draft implementation contract (C3A B3B). This slice is intentionally separate
from opening-allocation publication (B3C) and managed exposure mutation.

## Decision

`ManagedOpeningSourceVerifier` is the first trusted read path for an OPEN source
fact. It returns `VerifiedManagedOpeningSourceFact`, not
`VerifiedManagedOpeningAllocation`, and therefore cannot authorize allocation or
exposure changes.

The verifier reloads three independent sources while holding one account-scoped
fencing capability:

1. the committed `OperationRecord` and its result-derived history-deal binding;
2. the current `ObservationGraph` history-deal window; and
3. the durable `ManagedOpeningDealFact` records from B3A.

The verifier is constructed with a reviewed `ManagedOpeningQuantityDomain` for
the symbol/account. A caller query cannot replace that domain or select a new
logical-unit mapping.

No caller-supplied identity, digest, quantity, or authority flag is promoted to
proof. The existing `UnavailableManagedOpeningProvenanceSource` remains the
production allocation-provenance source until B3C supplies an allocation record.

The binding trust boundary is the private dispatch path: `validate_result()`
validates one broker result, `extract_bindings()` derives at most one
`fields.deal` anchor for an `order_send` result, and
`OneShotDispatchBackend::persist_result_with_bindings()` commits the raw result
and bindings atomically. B3B trusts only that committed binding contract; it
does not claim to independently parse or authenticate an opaque result payload.
An order-only result (`order != 0`, `deal == 0`) cannot satisfy this OPEN source
contract and remains unsupported/fail-closed.

The journal also stores `result_source_revision` in format v6. The field is set
only by the trusted result-persistence mutation and is preserved by later
lifecycle transitions. B3B requires this immutable anchor for both
`accepted` and `reconciling` records; legacy v1--v5 records decode with no
anchor but remain fail-closed for this verifier. The request revision still
identifies the current durable record, while the anchor identifies the exact
revision at which the broker result and binding first became durable.
Every B3A fact for that OPEN stores this anchor as its `operation_revision`;
the fact does not follow later lifecycle revisions.

## Ownership matrix

| Invariant | Owner | Not owned by this slice |
| --- | --- | --- |
| Journal operation identity and revision | `DurableJournalStore` reload plus exact comparison | caller query fields |
| Result-derived deal identity | committed reconciliation descriptor/binding | local fact alone |
| History completeness and account scope | `ObservationGraph` coverage and graph account | durable fact receipt |
| Deal/order/position/entry/direction | current history-deal observation | position snapshot alone |
| Exact broker quantity | B2 canonical quantity matcher | epsilon or decimal re-rounding |
| Immutable execution payload | B3A durable fact | current snapshot rewriting the fact |
| Cumulative execution frontier | `ManagedOpeningExecutionFrontier` | terminal OPEN state |
| Opening allocation publication | B3C | this verifier |

## Verification algorithm

For a valid `ManagedOpeningProvenanceRequest`, the verifier:

1. obtains the account lease and performs all reads inside its fencing
   capability;
2. reloads the exact journal key and revision, requiring a committed OPEN with
   a persisted result, a valid immutable result-source anchor, a lifecycle
   state of `accepted` or `reconciling`, and at least one result-derived
   `history_deal_present` binding;
3. requires a pre-dispatch `ReconciliationBaseline` from the same account and
   graph provenance. On the original graph instance every admitted deal must
   have first evidence newer than that baseline (the graph retains both first
   and latest observation revisions), and an exact complete-window coverage
   entry at the same revision. The newest complete observation whose window
   covers each deal timestamp must also contain that deal; a later covering
   sample that omits it is a contradiction even though historical evidence is
   retained in the graph. A repeated complete poll containing the unchanged
   deal may advance latest evidence without changing the immutable
   first-admission receipt. After
   restart numeric graph revisions are not compared across instances; the
   durable baseline/receipt provenance is retained and the re-observed graph
   must independently provide exact complete-window coverage;
4. reloads every bound deal and every deal for the bound order, requiring exact
   account/order/position identity, `DEAL_ENTRY_IN`, one direction, complete
   fields, and complete coverage;
5. scans durable B3A facts, requiring one exact fact for every authoritative
   deal, the immutable result-source revision and exact position, unchanged admission window,
   exact timestamp/direction, and canonical quantity equality;
6. builds the deterministic deal-ticket-sorted execution frontier, rejects an
   unknown requested volume or cumulative managed volume above it, and computes
   a domain-tagged FNV-64 fingerprint over the immutable result-source anchor,
   journal payload,
   descriptor/bindings, history fields, and canonical fact bytes; and
7. returns the sealed source fact only after every comparison succeeds.

Missing journal records remain `unavailable`; malformed records, missing facts,
history gaps, foreign orders/positions, rewritten payloads, quantity mismatch,
and duplicate/conflicting identities are `invalid_evidence`. Lease loss is
reported explicitly as `lease_not_held`. No positive result is synthesized from
the saved fact or a position snapshot.

## Restart and contradiction boundary

The verifier accepts a fresh graph instance after restart only when the durable
descriptor and first-admission receipts identify the original graph/account and
the new graph independently re-observes the same complete history window. The
graph instance identifier is provenance, never a cross-restart ordering
relation. A changed history payload, old coverage reused for a later uncovered
deal, a newer complete covering sample that omits a previously admitted deal,
missing causal evidence, foreign same-position execution, or unavailable
durable fact fails closed. A complete observation of a non-overlapping window
does not contradict the deal. The current journal store retains only its latest
revision, so B3B accepts a record already advanced into the `reconciling`
lifecycle only when the v6 immutable result-source anchor is present. A legacy
record without that anchor remains fail-closed.

The verifier does not infer that an OPEN is terminal, does not publish an
opening allocation, and does not modify `ManagedTradeOwner`, exposure, the C ABI,
or Trade Event Observation.

## Regression boundary

The B3B test suite covers one result-derived anchor plus history-discovered
partial fills, exact quantity and identity joins, pre-dispatch baseline
causality, stale-coverage rejection, missing and rewritten facts, foreign
same-position executions, stale journal revisions, graph restart, revoked
fencing, requested-volume bounds, invalid requests, durable-store I/O failure,
idempotent repeated polling, newer complete-history contradictions, stable
source identity across lifecycle changes and later partial fills, rejection of
fabricated future tickets, and a
production-shaped `OperationJournal` → `DispatchAdmissionBarrier` →
`OneShotDispatchBackend` → verifier path. B3C remains responsible for durable
opening-allocation publication and its crash matrix.

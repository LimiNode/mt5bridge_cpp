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
   a persisted result, a valid descriptor, and at least one result-derived
   `history_deal_present` binding;
3. requires one complete, common history window and verifies its coverage at
   the graph's current history-deal revision;
4. reloads every bound deal and every deal for the bound order, requiring exact
   account/order/position identity, `DEAL_ENTRY_IN`, one direction, complete
   fields, and complete coverage;
5. scans durable B3A facts, requiring one exact fact for every authoritative
   deal, exact operation revision and position, unchanged admission window,
   exact timestamp/direction, and canonical quantity equality;
6. builds the deterministic deal-ticket-sorted execution frontier and computes
   a domain-tagged digest over journal identity, history fields, and canonical
   fact bytes; and
7. returns the sealed source fact only after every comparison succeeds.

Missing journal records remain `unavailable`; malformed records, missing facts,
history gaps, foreign orders/positions, rewritten payloads, quantity mismatch,
and duplicate/conflicting identities are `invalid_evidence`. Lease loss is
reported explicitly as `lease_not_held`. No positive result is synthesized from
the saved fact or a position snapshot.

## Restart and contradiction boundary

The verifier accepts a fresh graph instance after restart when it independently
re-observes the same complete history window. The graph instance identifier in
the original B3A admission receipt is retained for provenance but is never used
as a global ordering relation. A changed history payload, missing coverage,
foreign same-position execution, or unavailable durable fact fails closed.

The verifier does not infer that an OPEN is terminal, does not publish an
opening allocation, and does not modify `ManagedTradeOwner`, exposure, the C ABI,
or Trade Event Observation.

## Regression boundary

The B3B test suite covers one and multiple partial fills, exact quantity and
identity joins, missing and rewritten facts, foreign same-position executions,
stale journal revisions, graph restart, revoked fencing, invalid requests, and
durable-store I/O failure. B3C remains responsible for durable opening-allocation
publication and its crash matrix.

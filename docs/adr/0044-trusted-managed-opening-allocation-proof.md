# ADR-0044: Trusted managed-opening allocation proof

## Status

Draft readiness contract for C3A B3D. This document fixes the lookup and
authority boundary before a producer implementation is added. It does not
enable production `INOUT`, mutate managed exposure, wire `ManagedTradeOwner`,
or change the public ABI.

## Context

ADR-0038 sealed `VerifiedManagedOpeningAllocation` but deliberately left its
producer unavailable. ADR-0042 supplies a fresh B3B source fact, and ADR-0043
persists one immutable allocation for each cumulative opening frontier.

The existing `ManagedOpeningProvenanceRequest` identifies an operation,
result-source revision, and broker `POSITION_IDENTIFIER`, but it does not
identify one frontier. That omission is harmless while the source is
unavailable and becomes unsafe when several immutable frontiers exist for one
OPEN. In particular, the durable frontier discriminator is allowed to advance
after an observation-graph restart; its numeric maximum is not a statement
that the corresponding record is the current cumulative frontier.

The producer therefore needs two explicit questions with different evidence
requirements:

* **exact historical lookup** — does this particular immutable allocation
  exist and remain source-valid?
* **current cumulative lookup** — which one valid allocation contains the
  complete cumulative source frontier currently supported by fresh B3B
  evidence?

An omitted selector must not silently choose between these questions.

## Decision

The B3D producer will use an explicit private lookup selector. The selector is
one of:

```text
ExactFrontier {
    ManagedOpeningAllocationKey key
}

CurrentFrontier {
    OperationKey operation_key
    uint64_t operation_revision       // immutable journal result-source revision
    uint64_t position_identifier
}
```

`ExactFrontier` compares the complete `(operation_key, operation_revision,
frontier_revision)` key and never falls back to an alias or another
discriminator. `CurrentFrontier` is the only path that may select a record;
it must select it by source completeness, not by the largest discriminator.
The existing query fields remain constraints and never become proof merely
because a caller supplied them.

One producer may implement both selectors because they share the same fenced
validation path. They remain separate variants in the private contract so a
caller cannot accidentally turn a historical replay into a currentness
claim.

## Current-frontier selection

For `CurrentFrontier`, the producer holds the account fencing capability and
reloads the complete allocation namespace. It then:

1. loads the exact committed OPEN journal record and requires its immutable
   `result_source_revision` to equal `operation_revision`;
2. scans every canonical `alloc-*` record for the operation and validates its
   journal, B3A fact references, immutable source tuple, quantities, digest,
   and canonical filename;
3. obtains a fresh `VerifiedManagedOpeningSourceFact` through
   `ManagedOpeningSourceVerifier` while the same fence is held;
4. requires one candidate whose fact-ticket set is exactly the fresh verified
   cumulative set, with matching position, order, direction, quantities, and
   source digest; and
5. rejects zero candidates, more than one candidate, an incomparable
   candidate, a missing historical ticket, or a source contradiction as
   `invalid_evidence`.

The current frontier is therefore the unique source-complete record. A larger
`frontier_revision` is only a durable discriminator and cannot establish
currentness. If a later cumulative frontier exists, the earlier record may
still be returned by `ExactFrontier`, but it must not be returned by
`CurrentFrontier`. If two records claim the same current source set with
different immutable payloads, the result is fail-closed rather than
arbitrarily ordered.

The producer must not infer currentness from a position snapshot, from the
latest filesystem filename, from `max(frontier_revision)`, or from a digest
without the referenced durable facts.

## Exact historical validation

For `ExactFrontier`, the producer loads only the requested canonical
`alloc-*` path and requires the decoded key to equal the requested key. It
then reloads the journal, all referenced B3A facts, and the immutable source
tuple. A missing record is `unavailable`; malformed, checksum-invalid,
non-canonical, source-mismatched, or incomplete evidence is
`invalid_evidence`. A valid historical allocation remains valid after later
compatible fills; later facts do not rewrite its ticket set or quantities.

The exact proof carries the selected frontier identity in the sealed value (at
minimum the complete allocation key or its `frontier_revision`), so downstream
recovery cannot confuse a historical proof with the current frontier. The
frontier identity is evidence of which immutable record was checked; it is not
permission to mutate exposure.

## Trusted proof construction

`VerifiedManagedOpeningAllocation` may be constructed only inside the B3D
producer after all of the following succeed under one account-scoped fencing
capability:

* the committed OPEN journal record is present, valid, and has the exact
  immutable result-source revision;
* the canonical `alloc-*` record is present and decodes to the requested key;
* every referenced B3A fact is present, version-supported, immutable, and
  exactly joined by deal ticket, account, order, position, direction,
  quantity, graph provenance, and coverage revision;
* the fresh B3B verifier agrees with the allocation's complete ticket set,
  position identity, immutable quantities, and evidence digest;
* cumulative broker and managed quantities are overflow-safe and equal to the
  validated frontier; and
* the held fencing capability remains valid at the return boundary.

Caller-provided digest, quantity, ticket list, position identity, frontier
revision, or authority flag can only constrain a lookup. None can construct or
upgrade a proof.

The producer returns three distinct trust facts and must not collapse them:

1. **durable allocation proof** — an immutable allocation and its durable
   source chain were reloaded and matched;
2. **current broker-evidence proof** — B3B freshly verified the complete
   history frontier selected by the request; and
3. **permission to mutate managed exposure** — not produced by B3D. A later
   owner/reconciliation slice must define that gate separately.

Possession of the first fact alone never authorizes exposure mutation.

## Fencing and restart

All reads that participate in a positive result occur while the account lease
and fencing capability are held. Lease loss returns `lease_not_held`; a stale
writer cannot publish, select, or return a positive proof after ownership has
changed. The producer performs no journal mutation and does not create a
second provenance registry.

After restart, graph-instance revisions are not compared numerically across
processes. Exact historical lookup relies on the immutable allocation's B3A
receipts and source revision. Current lookup requires a fresh B3B verification
from the current graph and the unique source-complete allocation; a record
whose historical facts are missing, rewritten, or contradicted is rejected.
The producer never reconstructs a lost allocation from the latest broker
position volume.

## Failure contract

The private result status keeps these cases distinct:

| Condition | Result |
| --- | --- |
| malformed selector | `invalid_request` |
| no exact record or no current source-complete record | `unavailable` |
| checksum, format, canonical-path, identity, quantity, or source mismatch | `invalid_evidence` |
| durable source read failure | `io_error` |
| lease/fencing capability lost | `lease_not_held` |
| valid allocation plus all required evidence | `found` with sealed proof |

An `unavailable` result is not repaired from a position snapshot or a
caller-supplied digest. An `invalid_evidence` result is not downgraded to
`unavailable` merely to make recovery retryable.

## B3D implementation boundary

The implementation may add only the private selector, producer, and focused
tests required by this contract. It must reuse the B3B verifier, B3A fact
store, B3C allocation store, journal validation, canonical path checks, and
account fencing already in the repository. It must not add a generic
provenance registry or a second durable store.

The implementation must not:

* wire the producer into `ManagedTradeOwner` or exposure accounting;
* enable managed production `INOUT` or any CLOSE path;
* alter terminal lifecycle semantics, the public ABI, or existing allocation
  formats; or
* infer ownership from a broker position snapshot, close ledger, staged
  preparation, or the maximum frontier discriminator.

## Required regressions

Before the producer can replace `UnavailableManagedOpeningProvenanceSource`,
the focused Windows Release tests must demonstrate:

1. exact lookup returns the requested older frontier after a later fill;
2. current lookup returns the unique cumulative frontier and does not select
   the record with the numerically greatest discriminator when its source set
   is older;
3. missing, rewritten, duplicated, aliased, or incomparable frontier records
   fail closed;
4. journal result-source revision, position identity, quantity, and all B3A
   references are checked against the fresh B3B proof;
5. restart with a new graph instance accepts only a fresh current-history
   verification and never compares local graph revisions across processes;
6. lease transfer between two writers prevents the stale writer from returning
   or publishing a positive result; and
7. fabricated caller identity, digest, quantity, ticket set, or frontier key
   cannot produce `found`.

The existing production `INOUT` fail-closed regression must remain unchanged.
The B3D test seam may use a real durable journal, B3A fact store, B3C
allocation store, and a controlled verifier fixture, but it must not weaken
the production trust boundary.

## Consequence and next step

This decision makes the expensive semantic choice explicit before code is
written: historical identity is exact-key lookup, while currentness is a
unique source-completeness relation. It avoids treating an implementation
discriminator as a causal clock and preserves immutable replay after later
fills.

The next bounded change is a separate B3D implementation PR. It should first
extend the private sealed proof with the selected frontier identity, then add
the producer and the negative regressions above. Until that PR is reviewed and
its exact-head Windows Release checks pass, the only production provenance
result remains `unavailable` and managed `INOUT` remains fail-closed.


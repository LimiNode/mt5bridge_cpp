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

The existing `ManagedOpeningProvenanceRequest` identifies an operation, one
journal revision for the B3B verifier, and broker `POSITION_IDENTIFIER`, but it
does not identify one frontier. That omission is harmless while the source is
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
    uint64_t result_source_revision   // immutable allocation/journal anchor
    uint64_t position_identifier
}
```

`ExactFrontier` compares the complete `(operation_key,
result_source_revision, frontier_revision)` allocation key and never falls
back to an alias or another discriminator. `CurrentFrontier` is the only path
that may select a record;
it must select it by source completeness, not by the largest discriminator.
The existing query fields remain constraints and never become proof merely
because a caller supplied them.

The B3D producer loads the current journal record before invoking B3B and
constructs a fresh `ManagedOpeningProvenanceRequest` with
`operation_revision = record.revision`. This preserves the existing B3B API:
its request revision is the current lifecycle revision, while the returned
`VerifiedManagedOpeningSourceFact::operation_revision()` and the allocation
key carry the immutable `result_source_revision`. B3D compares those two
anchors explicitly; it does not reinterpret the existing B3B field.

One producer may implement both selectors because they share the same fenced
validation path. They remain separate variants in the private contract so a
caller cannot accidentally turn a historical replay into a currentness
claim.

## Current-frontier selection

For `CurrentFrontier`, the producer holds the account fencing capability and
reloads the complete allocation namespace. It then:

1. loads the exact committed OPEN journal record and requires its immutable
   `result_source_revision` to equal the selector's
   `result_source_revision`;
2. scans every canonical `alloc-*` record for the operation and validates its
   journal, B3A fact references, immutable source tuple, quantities, digest,
   and canonical filename;
3. obtains a fresh `VerifiedManagedOpeningSourceFact` through
   `ManagedOpeningSourceVerifier` while the same fence is held;
4. requires one candidate whose fact-ticket set is exactly the fresh verified
   cumulative set, with matching position, immutable source tuple, quantities,
   and source digest; and
5. returns `unavailable` when no allocation has been published for the
   operation, or when the fresh proof is a compatible `extended` frontier for
   an already published prefix but its cumulative successor is not yet
   durable. This is normal publication lag, not a contradiction. A rewritten
   or incompatible candidate, a duplicate/alias, an incomparable candidate,
   a missing historical ticket, or a source contradiction is
   `invalid_evidence`. More than one current candidate is always
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

For `ExactFrontier`, the producer loads the requested canonical `alloc-*` path
and requires the decoded key to equal the requested key. It then performs the
existing account-wide `DurableManagedOpeningAllocationStore::scan()` under the
same fence. The scan is the namespace authority: it detects malformed sibling
records, duplicate identities, source-equivalent aliases, canonical-path
violations, and I/O failures without introducing a second scanner. A pending
`prep-*` record never produces a proof. The producer then reloads the journal,
all referenced B3A facts, and the immutable source tuple.

An absent requested allocation with a complete, contradiction-free scan is
`unavailable`; malformed, checksum-invalid, non-canonical, aliased,
source-mismatched, or incomplete evidence is `invalid_evidence`. A valid
historical allocation remains valid after later compatible fills; later facts
do not rewrite its ticket set or quantities. Fresh B3B evidence is compared to
the historical entry by the existing B3C relation: `exact` is accepted, and
`extended` is accepted when the fresh ticket set contains every historical
ticket and the cumulative quantity increases without changing the immutable
source tuple. The historical allocation's digest is retained; it is not
required to equal the digest of the expanded B3B frontier.

`CurrentFrontier` requires the stricter `exact` relation. Thus, after a later
fill, `ExactFrontier(A)` may succeed with an `extended` fresh B3B proof while
`CurrentFrontier` selects only the published B frontier.

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
* the fresh B3B verifier has been called with the current journal lifecycle
  revision and agrees with the immutable result-source anchor; its relation to
  the allocation is `exact` for `current` scope and `exact` or compatible
  `extended` for `historical` scope;
* cumulative broker and managed quantities are overflow-safe and equal to the
  validated frontier; and
* the held fencing capability remains valid at the return boundary.

Caller-provided digest, quantity, ticket list, position identity, frontier
revision, or authority flag can only constrain a lookup. None can construct or
upgrade a proof.

The future private sealed value contains at least:

```text
ManagedOpeningAllocationKey frontier_key
ManagedOpeningProofScope scope   // historical or current
OperationKey operation_key
uint64_t position_identifier
uint64_t managed_logical_units
uint64_t evidence_digest
```

`frontier_key` is the exact immutable allocation identity checked by the
producer. `scope` is a type-level semantic discriminator for downstream code;
it is not a claim that a historical proof is current. Adding these private
fields and the `lease_not_held` status is reserved for the separate B3D
implementation PR; this documentation PR does not change the existing B3B
interface or production contract.

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

## Historical/current semantics

The distinction is observable in the following bounded sequence. `r_s` is the
immutable `result_source_revision`; `d_A` and `d_B` are durable discriminators,
and `h_A`/`h_B` are the immutable allocation digests.

| Durable state | Fresh B3B tickets | Lookup | Expected result |
| --- | --- | --- | --- |
| `alloc-A`: `{a}`, `(r_s, d_A, h_A)` | `{a,b}` | `ExactFrontier(A)` | `found`, `historical`, `{a}`, `h_A`; B3B relation `extended` |
| `alloc-A`: `{a}`, `(r_s, d_A, h_A)` | `{a,b}` | `ExactFrontier(B)` | `unavailable` (B is not published) |
| `alloc-A`: `{a}`, `(r_s, d_A, h_A)` | `{a,b}` | `CurrentFrontier` | `unavailable` (compatible B is observed but not yet published) |
| `alloc-A` and `alloc-B`: `{a}`, `{a,b}` | `{a,b}` | `ExactFrontier(A)` | `found`, `historical`, `{a}`, `h_A` |
| `alloc-A` and `alloc-B`: `{a}`, `{a,b}` | `{a,b}` | `ExactFrontier(B)` | `found`, `historical`, `{a,b}`, `h_B` |
| `alloc-A` and `alloc-B`: `{a}`, `{a,b}` | `{a,b}` | `CurrentFrontier` | `found`, `current`, B, `{a,b}`, `h_B` |

The current result is not selected because `d_B` is numerically greater. It is
selected because B is the unique allocation whose complete immutable source
set exactly matches fresh B3B evidence. An alias, duplicate, or incomparable
candidate changes the result to `invalid_evidence`.

## Failure contract

The private result status keeps these cases distinct:

| Condition | Result |
| --- | --- |
| malformed selector | `invalid_request` |
| requested record is absent, or no allocation has yet been published and no contradiction exists | `unavailable` |
| checksum, format, canonical-path, identity, quantity, or source mismatch | `invalid_evidence` |
| durable source read failure | `io_error` |
| lease/fencing capability lost | `lease_not_held` |
| valid allocation plus all required evidence | `found` with sealed proof |

An `unavailable` result is not repaired from a position snapshot or a
caller-supplied digest. An `invalid_evidence` result is not downgraded to
`unavailable` merely to make recovery retryable.

The existing `ManagedOpeningProvenanceStatus` has no `lease_not_held` value.
That is an intentional private-contract extension for B3D, not a change to
the B3B verifier API: the B3D producer will add the status in its own result
type (or an equivalent strict wrapper) and map lease loss explicitly.

## B3D implementation readiness matrix

| Concern | Reuse | B3D obligation | Observable regression |
| --- | --- | --- | --- |
| Journal/source anchor | `DurableJournalStore`, v6 `result_source_revision` | Separate current lifecycle revision from immutable allocation identity | journal advances; B3B receives current revision while proof keeps `r_s` |
| Fresh broker evidence | `ManagedOpeningSourceVerifier` | Accept `exact`/`extended` only for historical scope; `exact` only for current | A survives B; current selects B |
| Allocation namespace | `DurableManagedOpeningAllocationStore::scan()` and `load()` | Scan before `found`; pending prep never proves; aliases fail closed | malformed sibling and source-equivalent alias |
| Durable source chain | B3A fact store and B3C source validation | Match every referenced fact and cumulative quantity without reconstruction | missing/rewritten fact and quantity mismatch |
| Proof boundary | private sealed constructor | Include exact key and `historical`/`current` scope; no lease lifetime claim | fabricated caller fields cannot yield `found` |
| Fencing | `SingleWriterLease` | Hold capability through all reads and return `lease_not_held` on loss | lease transfer blocks stale writer |
| Recovery | existing real Windows stores | No second store or provenance registry; restart rechecks sources | restart with new graph instance |

The implementation review must also include an anti-neuroslop check: each
positive assertion in the tests must name the durable record, source relation,
fence state, or exact status it proves; no test may pass merely because a
non-zero digest, a maximum discriminator, a position snapshot, or a caller
selected identity is present.

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


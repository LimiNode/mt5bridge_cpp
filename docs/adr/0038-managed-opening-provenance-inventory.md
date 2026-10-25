# ADR-0038: Managed opening provenance inventory and trust boundary

## Status

Proposed as C3A PR A, the provenance-inventory prerequisite for a future
durable managed-position ownership source. This decision does not add an
opening-allocation store, enable production `INOUT`, or make the structural
frontier contract authoritative.

## Context

ADR-0037 deliberately introduced a private structural contract for an exact
pre-position ownership frontier. Its entries contain an operation identity,
revision, logical units, broker volume, and an evidence digest, but those
fields only describe a candidate record. A caller can construct the same
shape without proving that the operation was a committed managed `OPEN` or
that any opening allocation was published.

The next implementation must therefore begin with an inventory of the facts
that already exist in the repository. A non-zero digest, a valid
`OperationKey`, or a broker position volume is an identifier or observation;
none is an authority proof by itself. The inventory also prevents a close
intent or the C2 candidate from being used as a reverse-derived ownership
source.

## Evidence matrix

| Durable or observed source | What it proves | What it does not prove | Sufficient for committed OPEN ownership? |
| --- | --- | --- | --- |
| OPEN `OperationRecord` in the journal | Account-scoped operation identity, immutable request, lifecycle, revision, fencing, and any result-derived bindings | That a broker position currently contains this operation, that a broker opening allocation was committed, or that logical units were consumed into a position | No |
| Result-derived reconciliation bindings | A persisted link from a result predicate to broker tickets/identifiers covered by the descriptor | Managed logical units, exclusive ownership, allocation publication, or current position continuity after external mutation | No |
| Reconciliation graph/history observations | Observation revisions, deal identity, position identifier, and provenance for the sampled broker facts | Durable ownership, a committed managed allocation, or semantic attribution to one `TradeId` | No |
| `ManagedDealSettlement` and settlement frontiers | Immutable per-deal managed facts and cumulative close settlement for the current producer contract | An opening allocation; the producer currently derives close settlement only and accepts owner-supplied logical units | No |
| `ManagedAllocationEntry` ledger | Immutable consumption of a broker close leg, with source validation, capacity, and replay identity | An opening leg or a proof that a netting position belongs to one managed trade | No |
| Broker allocation/reversal envelope | Broker-only identity, close capacity, and reversal provenance | Managed ownership or logical units; broker volume is never a source of managed units | No |
| `POSITION_IDENTIFIER` | Stable broker position identity for an exact observation key | Which managed operation owns the position or whether external residue exists | No |
| Account fencing and journal CAS | Writer serialization, monotonic revisions, and atomic publication boundaries | Semantic provenance of the data being published | No |
| Historical recovery scan | That previously committed records still match their durable source evidence | Creation of missing history, attribution of a legacy position, or repair from the latest broker volume | No |

The only acceptable future authority is a durable chain that joins all of the
following under one account-scoped fencing domain:

```text
committed OPEN journal record
        + result-derived opening execution binding
        + committed opening allocation
        + immutable broker position identifier
        + durable consumption / external-mutation history
        + exact replay and recovery validation
        -> verified managed opening allocation
```

The chain must be independently reloadable after restart. A digest records a
canonical identity for that chain; it does not create the chain.

## Trust-boundary contract

The private C3A contract is `managed_opening_provenance_contract.hpp`. It
defines:

- a lookup request whose operation, revision, and position identifier are
  query constraints, not evidence supplied by the caller;
- a sealed `VerifiedManagedOpeningAllocation` value whose constructor is not
  available to callers, fixtures, or the structural C2 candidate;
- a status-bearing durable-source interface; and
- an explicit unavailable result for the current repository state.

No implementation in this slice can construct the sealed proof. A future
producer may do so only after it has independently validated the journal,
opening allocation, result binding, position identity, and committed
consumption history. `found()` checks only the local shape of a returned
sealed value; it does not compare the value with the lookup request or confer
authority on its own. A future producer must compare the exact operation key,
revision, and `POSITION_IDENTIFIER` with independently reloaded durable
records before returning `found`. Returning `found` without the sealed value
is invalid. There is intentionally no `is_authoritative` flag, caller-provided
signature, or trust bit that upgrades a structural entry.

The existing `DurableManagedPositionOwnershipSource` remains a structural
frontier lookup used by the ADR-0037 contract test. It is not changed into an
authority source by this ADR. Until a genuine producer exists, production
`INOUT` attribution and allocation remain fail-closed.

## Required negative cases

The contract and later durable implementation must reject:

- a forged OPEN identity, revision, or non-zero digest with no committed
  source;
- an operation whose kind is not `open`, or an uncommitted/invalid journal
  record;
- an opening execution binding that does not match the account, operation, or
  broker `POSITION_IDENTIFIER`;
- a missing or conflicting opening allocation;
- duplicate or conflicting source replay;
- checksum, truncation, or incomplete-file corruption;
- stale fencing at publication; and
- a legacy position for which the complete chain is unavailable.

Broker volume, close intent, staged ownership, and the C2 candidate must not
be accepted as substitutes for any of these facts.

## Scope and deferred work

This PR is inventory and contract only. It does not add durable files, opening
allocation formats, owner wiring, exposure mutation, journal CAS, broker
dispatch, partial/multi-owner attribution, FIFO/LIFO/pro-rata policy, or
legacy-position migration. Those belong to separately reviewable C3A PRs and
must use real file fixtures once a genuine source is available.

If the source inventory cannot be completed from existing data, the correct
result is `unavailable` and a documented implementation blocker, not a
synthetic authority record.

## Verification boundary

The C3A contract test verifies that:

1. a structurally valid lookup request does not itself produce a proof;
2. the current unavailable source returns fail-closed without a sealed value;
3. a result marked `found` without a sealed value is rejected; and
4. changing caller-supplied query identity still returns `unavailable`; and
5. the sealed proof cannot be constructed from caller-supplied identity,
   revision, units, or digest.

Positive durable opening-authority tests are intentionally deferred until the
source implementation exists. The ADR-0037 in-memory frontier fixture remains
useful for structural attribution rules but is not evidence of production
authority.

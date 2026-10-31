# ADR-0043: Immutable managed-opening allocation frontiers

## Status

Draft implementation contract (C3A B3C). This slice publishes durable
source-derived OPEN allocations only; it does not mutate managed exposure,
wire `ManagedTradeOwner`, enable production `INOUT`, or change the public ABI.

## Decision

Each cumulative opening execution frontier is persisted as a separate
immutable record. Its identity is the account-scoped operation key, the
immutable journal `result_source_revision`, and the frontier's latest durable
fact revision. The payload also retains the broker position and order
identities, exact normalized broker and managed quantities, the B3B evidence
digest, and the complete sorted set of B3A deal-fact references.

The store has two files for one frontier:

* `prep-*` is a retryable preparation manifest written after B3B source
  verification;
* `alloc-*` is the final immutable allocation publication.

Both files use bounded checksummed envelopes and atomic write/replace under an
account-scoped writer lease and a store lock. A second identical preparation or
publication is idempotent. A different payload for the same identity is a
conflict. A later partial fill creates a new frontier record and never rewrites
the previous one. Recovery of an older frontier remains valid when the fact
store also contains later compatible facts: omitted facts must come from a
strictly newer admission revision, otherwise the historical snapshot is
incomplete and fails closed.

## Source boundary

The publisher accepts only `VerifiedManagedOpeningSourceFact` produced by
`ManagedOpeningSourceVerifier`. Caller-supplied position, quantity, digest, or
deal-ticket values are never treated as authority. Before either preparation
or publication the store reloads the exact OPEN journal record and all durable
B3A facts, checks the immutable source tuple, fact references, quantity domain,
frontier revision, and cumulative sums. Final publication also requires the
store-owned `ManagedOpeningSourceVerifier` to re-check the current observation
graph while the account fence is held; a proof supplied by a caller is never
enough to create an `alloc-*` record. Recovery scans perform the same durable
structural/source checks without manufacturing a sealed proof object.

The scan path is fail-closed for malformed records, missing facts, foreign
operation identities, invalid journal sources, reparse points, and I/O errors.
An absent account directory is an empty complete scan. A pending preparation is
retained across restart and is finalized only after a fresh B3B verification.

The durable `ManagedOpeningAllocationEntry` remains a storage record, not a
`VerifiedManagedOpeningAllocation`. No trusted producer exists in B3C. A future
producer must reload the allocation, journal result, and B3A facts and rerun
B3B; its proof revision denotes the immutable journal result-source revision,
not a later lifecycle revision. Caller-supplied digest, quantity, or local
record values cannot construct that proof.

## Crash and replay rules

The durable order is:

```text
verify sources -> write and flush prep-* -> verify again -> write and flush alloc-*
                                      \-> remove prep-* (best effort)
```

A crash before final publication leaves `prep-*` and permits retry. A crash
after `alloc-*` but before removing `prep-*` is harmless: replay compares the
complete manifest and returns `already_committed`. A restart after successful
publication can replay from `alloc-*` even when the preparation file is gone.

The account lease remains held across each publication callback. Loss of the
lease returns `lease_not_held`; no stale writer is allowed to publish.

## Compatibility and scope

The format is versioned as `MT5OAL01`. Unknown versions, oversized bodies,
checksum failures, duplicate or unsorted deal references, and quantity
overflow are rejected. No legacy allocation record is migrated automatically.
The CLOSE allocation ledger and all production exposure paths remain unchanged.

## Regression boundary

The focused Windows test covers prepare/publish, exact replay, restart
recovery, missing-source rejection, unknown-account empty scans, stale lease
behavior, stale-proof rejection at the low-level store boundary, canonical
reserved-path and pending-manifest handling, a real journal/fact/allocation
filesystem restart, and preservation of an earlier frontier after a second
partial fill.
Additional B3C tests must retain the same fail-closed source and fencing
boundary when later owner-loop slices are implemented.

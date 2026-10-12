# ADR-0035: Durable managed allocation ledger

## Status

Proposed as the next bounded managed-allocation slice. The source proofs and
read-only admission from ADR-0034 remain implemented; this decision defines
the durable claim boundary that may consume a broker close leg. It does not
yet add an allocation store or owner-loop integration.

## Context

ADR-0034 admits a canonical `ManagedBrokerVolumeMapping` only after reloading
the immutable managed settlement fact, broker reversal, ownership basis,
operation record, and broker-only envelope. That admission is deliberately
read-only. A caller may perform the admission and be descheduled before it
tries to persist an allocation, so an admitted mapping is not a reservation.

The broker envelope still contains the complete close leg as unallocated. A
future allocation layer must therefore establish one serialized publication
point that prevents two concurrent callers from consuming the same broker
volume. It must also preserve immutable per-deal facts and remain independent
of FIFO, LIFO, pro-rata, and managed-exposure mutation.

## Decision

Introduce a durable allocation entry keyed by the exact source identities:

```text
(BrokerReversalKey, ManagedDealSettlementKey, broker_leg)
```

The first implementation supports only `broker_close_leg`. The reverse-open
leg remains explicitly unallocated and cannot be assigned by this slice.
`ManagedDealSettlementKey` already contains `(OperationKey, DEAL_TICKET)`, so
the key retains both the managed `TradeId`/operation identity and the exact
broker deal identity.

Each entry stores the canonical mapping fields and their source provenance:

```text
account
broker reversal key
managed settlement key
broker leg = close
managed logical units
exact mapped broker volume
managed settlement provenance
broker reversal provenance
settlement.source_operation_revision
basis.operation_revision
```

An entry means:

```text
this exact managed per-deal fact consumes this exact step-normalized quantity
from this broker reversal's close leg
```

It does not rewrite the immutable broker envelope and does not mutate managed
exposure. Remaining close-leg availability is derived from the committed
entries for the broker key:

```text
remaining_close = envelope.broker_close_leg
                  - sum(committed close entries)
```

All subtraction and summation occur in the envelope's exact integer
`BrokerVolume` domain. A commit is rejected if scales or step units differ,
if addition would overflow, or if the new entry would exceed the durable
broker close leg.

## Atomic commit boundary

The allocation store owns one exclusive lock for its namespace. The caller
must also hold the account-scoped single-writer lease (or an equivalent
fencing capability) that protects the journal and reconciliation sources.
This is an authoritative commit capability, not merely a snapshot of the
current token: the lease/fencing implementation must make a stale writer
unable to publish after ownership is lost. Ownership must remain continuous
from source revalidation through allocation publication; the namespace lock
alone does not serialize a concurrent journal revision.

The ordering is therefore explicit: obtain the authoritative fencing
capability, acquire the allocation lock, and keep both through the final
publication decision.

Its commit operation must perform the following while holding the allocation
lock and the source-writer ownership boundary:

1. Read and historically validate every existing allocation entry for the
   broker key. This validation uses the entry's immutable identity,
   provenance, immutable source records, duplicate rules, and capacity; it
   does not require the current journal revision to equal an older entry's
   pinned revision.
2. Look up the exact candidate key. If it exists with the same immutable
   payload, return `already_committed`. If it exists with a different payload,
   return `conflict`. Neither path runs a fresh ADR-0034 admission.
3. Only when the exact key is absent, revalidate the candidate mapping against
   the durable source stores using the current committed-only admission rules
   from ADR-0034. A mapping admitted by a previous read is never accepted as a
   reservation token for a new entry.
4. Reject the new candidate when the exact close-leg sum would exceed the
   broker reversal proof.
5. Publish one immutable entry through the existing temp-file,
   flush/replace/write-through pattern. The atomic file publication is the
   commit point for that entry.

The entry pins two different operation revisions: the per-deal settlement's
`source_operation_revision` and the ownership basis's `operation_revision`.
The latter may be newer for an incremental settlement, but it must satisfy
the durable relation `settlement.source_operation_revision <=
basis.operation_revision` at commit time. Neither revision is silently
rewritten on replay. The allocation lock serializes allocation writers, while
the account writer lease/fencing boundary serializes them with source updates;
it is not a replacement for the source stores' own durability locks.

The concrete store seam must therefore accept or otherwise verify this
authoritative fencing capability as part of `commit`. A bare call that reads a
non-zero `SingleWriterLease::held_fencing_token(account)` and then continues
without a durable fencing guarantee is insufficient. The implementation must
have a check that remains authoritative through the single-entry publication
point; a test double must model lease loss between admission and publication
and require the stale writer to fail closed.

## Commit-time admission versus historical replay

The source-coherence admission in ADR-0034 is a commit-time requirement. It
proves that the candidate was authorized when the allocation entry was
published. Once published, the entry is an immutable historical fact and must
not be reinterpreted through the current mutable journal revision.

In particular, restart scan must not call the current
`validate_managed_ownership_basis()` contract in a way that requires:

```text
current journal revision == entry.basis.operation_revision
```

An operation may legitimately advance from revision 8 to revision 11 after an
entry was committed at revision 8. That is not corruption, re-attribution, or
a reason to remove the earlier capacity consumption. Historical scan instead
validates the entry's own checksum, filename, immutable identity and
provenance fields, duplicate rules, and capacity against the durable broker
envelope. It may cross-check immutable settlement, broker-reversal, and raw
ownership-basis records, but it must not reject a valid historical entry merely
because the mutable journal now exposes a later revision.

If a future design needs to independently re-prove the historical operation
after its journal revision has advanced, it must persist sufficient immutable
operation evidence (for example, a versioned operation snapshot or digest) in
the allocation record. That is a separate extension; this slice treats the
successful fenced commit itself as the durable fact that admission passed.

## Durable recovery and replay

Entries are independent immutable records in a dedicated namespace and use a
versioned, checksummed envelope. A crash before file publication leaves no
allocation entry; a crash after publication leaves the complete entry for
restart scan. Temporary files are ignored or rejected according to the
existing file-store policy and never count as committed allocation.

Restart scan is all-or-nothing. It rejects malformed records, filename/key
mismatches, duplicate keys, invalid provenance, mismatches against immutable
settlement/broker/basis/envelope records, duplicate allocation of the same
managed settlement key, and sums that exceed the durable broker close leg. It
does not treat a later mutable journal revision as a historical source
mismatch. Exact replay is `already_committed`; a same-key different payload is
`conflict`.

## Fail-closed invariants

- Only a valid `ManagedBrokerVolumeMapping` admitted from durable sources can
  create an entry.
- The entry's account, broker key, managed key, mapped volume, and provenance
  must equal the re-derived durable mapping.
- `settlement.source_operation_revision` is non-zero and no greater than the
  separately stored `basis.operation_revision`.
- Only `broker_close_leg` is allocatable in this slice. Any reverse-open claim
  is rejected as unsupported rather than inferred from an `INOUT` reversal.
- The sum of all committed close entries for one broker deal never exceeds
  `BrokerReversalRecord::broker_close_leg`.
- One allocation key has at most one immutable payload; exact replay is
  idempotent and re-attribution conflicts.
- A managed settlement fact cannot be committed twice under different broker
  identities because the mapping identity requires the same `DEAL_TICKET`.
- No allocation commit changes `BrokerAllocationEnvelope`, journal records,
  settlement facts, ownership bases, or `ManagedTradeState`.
- Read-only admission remains safe to repeat, but only the allocation-store
  commit under its lock consumes derived remaining capacity.
- Allocation commit requires the same account-scoped writer ownership (or
  equivalent authoritative fencing capability) that protects the pinned
  operation sources; a ledger lock or a one-time token read without that
  source boundary is insufficient.
- FIFO/LIFO/pro-rata policy, cross-operation ordering, and owner-loop wiring
  remain outside this slice.

## Consequences

The broker envelope remains a stable source proof while the allocation ledger
becomes the serialized consumer state. Multiple operation-scoped entries may
share one broker close leg until its exact integer capacity is exhausted, and
concurrent duplicate writers converge to replay or conflict instead of double
consumption. The design also leaves the reverse-open leg available for a
separate managed-open proof rather than guessing ownership from reversal
geometry.

The store must revalidate source proofs during commit, so the first
implementation may be more expensive than the current `load()` path. That is
intentional; latency measurement and indexing are later optimizations, not
reasons to weaken the atomic admission boundary.

## Verification boundary for the implementation slice

The future implementation must cover at least:

- exact durable entry round-trip and restart scan;
- exact replay and same-key conflicting replay;
- two distinct operation keys sharing one broker close leg;
- aggregate capacity exhaustion and integer overflow rejection;
- concurrent-style repeated commit proving one winner and idempotent replay;
- stale, missing, pending-only, and mismatched source proofs rejected during
  commit;
- historical replay after journal advancement: an entry committed at revision
  8 remains valid and counts toward capacity when the current operation record
  is at revision 11;
- fencing handoff: a writer that loses ownership after admission but before
  publication cannot publish a stale entry, while the new fenced writer can
  commit or replay it;
- reverse-open allocation rejected;
- crash before and after the single-entry atomic publication point; and
- no changes to managed exposure or existing broker-envelope bytes.

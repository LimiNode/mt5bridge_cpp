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
source operation revision
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
fencing token) that protects the journal and reconciliation sources. That
ownership must remain continuous from source revalidation through allocation
publication; the namespace lock alone does not serialize a concurrent journal
revision.

Its commit operation must perform all of the following while holding the
allocation lock and the source-writer ownership boundary:

1. Revalidate the candidate mapping against the durable source stores using
   the committed-only admission rules from ADR-0034. A mapping admitted by a
   previous read is never accepted as a reservation token.
2. Scan and validate every existing allocation entry for the broker key.
3. Treat an exact existing entry as an idempotent replay.
4. Reject a different entry with the same composite key as a conflict.
5. Reject the candidate when the exact close-leg sum would exceed the broker
   reversal proof.
6. Publish one immutable entry through the existing temp-file,
   flush/replace/write-through pattern. The atomic file publication is the
   commit point for that entry.

The entry pins the exact operation revision and source provenances used by the
revalidation. A changed or missing source fails closed; the store never
silently refreshes an entry to a later operation revision or a different
provenance. The allocation lock serializes allocation writers, while the
account writer lease/fencing boundary serializes them with source updates; it
is not a replacement for the source stores' own durability locks.

## Durable recovery and replay

Entries are independent immutable records in a dedicated namespace and use a
versioned, checksummed envelope. A crash before file publication leaves no
allocation entry; a crash after publication leaves the complete entry for
restart scan. Temporary files are ignored or rejected according to the
existing file-store policy and never count as committed allocation.

Restart scan is all-or-nothing. It rejects malformed records, filename/key
mismatches, duplicate keys, invalid provenance, source mismatches, duplicate
allocation of the same managed settlement key, and sums that exceed the
durable broker close leg. Exact replay is `already_committed`; a same-key
different payload is `conflict`.

## Fail-closed invariants

- Only a valid `ManagedBrokerVolumeMapping` admitted from durable sources can
  create an entry.
- The entry's account, broker key, managed key, mapped volume, and provenance
  must equal the re-derived durable mapping.
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
  equivalent fencing proof) that protects the pinned operation sources; a
  ledger lock without that source boundary is insufficient.
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
- reverse-open allocation rejected;
- crash before and after the single-entry atomic publication point; and
- no changes to managed exposure or existing broker-envelope bytes.

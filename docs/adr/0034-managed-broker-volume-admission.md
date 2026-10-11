# ADR-0034: Durable admission for managed broker-volume mappings

## Status

Accepted as a bounded read-only source-coherence slice after ADR-0033. The
slice admits an exact mapping only when its immutable managed settlement,
broker reversal, ownership basis, operation record, and broker-only envelope
are all present and mutually consistent. It does not reserve volume or write
an allocation entry.

## Context

ADR-0033 defines exact integer conversion from an immutable managed per-deal
fact to the broker scale and step proved by a durable `DEAL_ENTRY_INOUT`
reversal. That conversion alone is not enough to authorize a future allocation:

```text
structurally valid mapping
    !=
authoritative durable source coherence
```

A caller could otherwise derive a mapping from copied or stale in-memory
records, use a broker row that is not tied to the managed operation, or treat a
consumed broker envelope as still available. The next boundary must therefore
re-load the source records from their durable stores and compare the exact
identity, provenance, revision, and quantity fields.

## Decision

`admit_managed_broker_volume_mapping()` is a read-only gate. It accepts a
candidate only when all of the following checks pass:

1. The candidate is locally valid.
2. The exact `ManagedDealSettlementKey` loads a valid immutable fact whose
   operation/deal identity, managed units, and provenance equal the candidate.
3. The exact `BrokerReversalKey` loads a valid `BrokerReversalRecord` for the
   same account and provenance, and the record is explicitly
   `DEAL_ENTRY_INOUT` evidence.
4. Re-deriving the mapping from those two loaded durable records produces the
   candidate exactly, including broker scale, step, and deal-volume bound.
5. The composite `(BrokerReversalKey, OperationKey)` loads a valid
   `ManagedOwnershipBasis` whose broker provenance and pinned operation
   revision match the settlement fact.
6. The pinned operation record is present, valid, still the same revision, is
   a settled close, and authorizes no more managed units than its durable
   `settled_volume`.
7. The ownership-basis proof revalidates its broker-only allocation envelope.
   Both broker legs must still equal their original unallocated legs.

On success the gate returns a canonical mapping re-derived from the durable
managed and broker facts. Missing, malformed, stale, mismatched, or consumed
sources fail closed with a status that identifies the failed boundary.

## Guarantees and limits

The admitted result proves source coherence for a later allocation step. It
does not:

- reserve either broker leg;
- decrement an unallocated envelope;
- create a durable allocation entry;
- select FIFO, LIFO, pro-rata, or another allocation policy;
- mutate managed exposure or owner state;
- prevent two concurrent callers from admitting the same candidate.

Duplicate-attribution prevention requires one subsequent durable allocation
ledger with an atomic immutable-entry/idempotency boundary. The admission gate
must not be presented as that ledger or as an ownership decision.

The gate also does not invent a cross-store transaction. Its inputs are
durable source seams, and the next ledger slice remains responsible for
serializing the admission-to-allocation transition against concurrent writers.

## Explicit non-goals

- no public `TradeManager` or owner-loop integration;
- no broker-leg mutation or managed exposure mutation;
- no new logical scale or physical-unit contract;
- no durable mapping or allocation record;
- no recovery protocol beyond the existing source-store scans;
- no reinterpretation of a structurally valid caller-supplied mapping as proof.

## Verification boundary

Focused regressions cover successful admission from all durable sources,
candidate bypass of a changed durable settlement fact, missing ownership basis,
consumed broker envelope, and operation-revision mismatch. Header
self-containment also compiles the new public contract independently.

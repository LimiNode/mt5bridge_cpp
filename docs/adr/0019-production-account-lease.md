# ADR-0019: Production account lease and durable fencing epoch

## Status

Accepted for the native durable-dispatch layer. This slice does not create a
TradeManager or expose a public `order_send` entry point.

## Decision

The production Windows owner uses an account-scoped OS lock and a durable
monotonic fencing epoch. `WindowsSingleWriterLease` acquires an exclusive lock
file below the journal directory, reads the last valid epoch, increments it,
and replaces the epoch file with a flushed, write-through update before
reporting the lease ready.

The lease exposes the resulting non-zero token through the existing
`SingleWriterLease` seam. The token is returned only for the exact immutable
`AccountKey` supplied at construction and remains valid only while the lease
object owns its Windows handle. A second owner for the same account cannot
acquire the lock; after release or process recovery, the next owner receives a
strictly higher token.

Lease metadata is separate from operation records:

```text
lease-<hex server>-<hex login>.lock   exclusive OS ownership handle
lease-<hex server>-<hex login>.epoch  durable little-endian uint64 epoch
```

The account identity is encoded losslessly in the filename rather than using a
hash-only name, so distinct server names cannot silently share a fencing lock.
An absent epoch starts at one. A malformed, zero, or exhausted epoch fails
closed; a stale temporary epoch file is ignored and replaced while the owner
lock is held.

The lease is held continuously across admission and the private one-shot
backend. The journal still remains the authority for operation revision and
`dispatching` state; the lease token is fencing evidence, not a replacement for
the journal CAS.

## Consequences

- Two live owners cannot submit the same `AccountKey` concurrently through the
  production lease implementation.
- Fencing tokens survive process restart and are never reused after a clean
  release or crash recovery.
- Corrupt lease metadata is not interpreted as a fresh epoch and cannot reopen
  dispatch.
- The public C ABI and the private Python transport remain unchanged.
- A future manager must retain the lease object through the final account and
  lease checks immediately adjacent to the backend call.

## Verification

`tests/file_journal_store_test.cpp` verifies exclusive same-account ownership,
account scoping, monotonic token advancement after release, and fail-closed
handling of a corrupt epoch file. The test uses the same anchored directory as
the Windows durable journal store and does not invoke Python or `order_send`.

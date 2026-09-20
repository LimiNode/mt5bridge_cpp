# ADR-0012: Windows file-backed durable journal store

## Status

Accepted. This slice supplies the concrete durable store for the admission
barrier; it still does not invoke or expose `order_send`.

## Context

ADR-0011 defined `DurableJournalStore` as a replaceable compare-and-commit
seam and used an in-memory implementation as a contract double. A real owner
loop needs the same CAS and crash boundary after a process restart. The store
must not overwrite a record it cannot validate, and it must not report a
successful commit before the replacement is flushed and atomically visible.

## Decision

`mt5bridge::WindowsFileJournalStore` is a small native static-library target,
separate from both `mt5bridge::client` and the CPython-backed `mt5_bridge.dll`.
It is available to the managed owner loop without introducing a Python or C
ABI dependency.

The store uses one directory containing:

- one binary record per `OperationKey`;
- one `.journal.lock` file used with an exclusive Win32 byte-range lock.

Every record has a versioned envelope, bounded lengths, and an FNV-1a
checksum over its serialized body. The body contains the immutable
`AccountKey`, managed IDs, request/result payloads, both state enums, revision,
and fencing token. Recovery accepts a record only when the envelope, checksum,
identity, and `OperationRecord::valid()` contract all pass. Missing records are
the only normal empty result; malformed records fail closed and are never
overwritten.

`commit(record, expected_revision)` performs the following while holding the
directory lock:

1. validate the candidate and require revision `1` for creation or exactly
   `expected_revision + 1` for an update;
2. read and validate the existing target, rejecting a mismatch as `conflict`
   and corruption/I/O as `io_error`;
3. write a bounded temporary file, call `FlushFileBuffers`, close it, and
   atomically replace the target with `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`.

The owner-loop cache is still updated only by `OperationJournal` after this
method returns `committed`. The file format is an implementation detail and is
not part of the C ABI; future stores may use another medium behind the same
interface.

## Consequences

- Two owner processes cannot pass the same store CAS concurrently.
- A crash before the atomic rename leaves the previous complete record intact;
  a crash after rename exposes either the old or the fully flushed new record.
- Truncation, checksum failure, unknown state values, oversized payloads, and
  filename collisions are fail-closed rather than interpreted as absence.
- Record files are keyed by a stable hash of `AccountKey.server` plus login and
  managed IDs; the serialized identity is still checked, so a hash collision
  cannot alias another operation.
- The store is Windows-specific and does not change the public C ABI or add a
  public unmanaged trade-send method.

## Verification

`tests/file_journal_store_test.cpp` covers directory creation, durable create
and recovery, stale-writer CAS conflicts across two store instances, atomic
result persistence and reopen, duplicate creation, and corruption rejection
without overwrite. The test uses only a private temporary directory and never
loads Python, contacts MT5, or calls `order_send`.

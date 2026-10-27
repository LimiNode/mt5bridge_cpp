# ADR-0041: Durable managed opening source facts

Status: Draft implementation contract (C3A B3A)

## Decision

Persist each locally validated `ManagedOpeningDealFact` as one immutable,
account-scoped filesystem record keyed by `AccountKey + DEAL_TICKET`. This
slice provides durability, deterministic decoding, corruption detection, and
idempotent replay. It does not establish broker or journal authority and does
not publish an opening allocation.

The implementation uses the existing Windows file-store primitives:

- a flushed temporary file followed by `MoveFileExW(...WRITE_THROUGH)`;
- the existing account-scoped `SingleWriterLease` fencing callback;
- the existing bounded little-endian file envelope and checksum helper;
- a store-local file lock to serialize same-root writers.

The checksum detects accidental record corruption only. It is not a source
authority proof and cannot replace independent journal or broker validation.

## Ownership matrix

| Invariant | Owner in this slice | Explicitly not owned here |
| --- | --- | --- |
| Fact field shape and quantity consistency | `ManagedOpeningDealFact::valid()` and canonical decoder | Broker/journal existence |
| Canonical byte order and schema | `canonical_bytes()` / `from_canonical_bytes()` | Authentication or provenance authority |
| Record envelope bounds and corruption detection | `WindowsFileManagedOpeningFactStore` | Truth of the source observation |
| First receipt preservation and exact replay | Store commit/load path | Freshness across graph instances |
| Account + deal-ticket namespace isolation | Canonical length-delimited key, account subdirectory, and decoded identity check | Allocation capacity and exposure |
| Atomic durable publication | Existing file writer + `MoveFileExW` under lease | Opening allocation publication |
| Source freshness and producer trust | Future authoritative producer (B3B) | This store |
| Lifecycle terminality | Future lifecycle proof | C3A frontier/fact store |

## Record format

The durable envelope is `MT5OPN01` version `1`, followed by a little-endian
body length and checksum. The body contains the account server, account login,
deal ticket, and the exact `ManagedOpeningDealFact::canonical_bytes()` payload.
The filename namespace uses explicit length-delimited server bytes followed by
fixed-width login and deal-ticket fields; it never relies on text concatenation
or embedded NUL separators. The root contains deterministic `account-<digest>`
subdirectories, and each account scan reads only its own subdirectory. A digest
collision is detected by the decoded identity/path check and fails closed. Thus
corruption in another account namespace does not block this account, while any
unknown, foreign, or malformed record in the requested namespace does.

The canonical fact payload has one shared maximum of 1 MiB enforced by fact
construction, canonical decoding, and the durable envelope encoder/decoder.
The decoder rejects unknown versions, truncation, trailing bytes, invalid
enums, invalid quantity fields, identity mismatch, and non-canonical payloads.

`encode(decode(canonical_bytes)) == canonical_bytes` is required for the fact
payload. The store never reconstructs quantity from floating-point values.

Temporary `.tmp` files are not records. A crash before the atomic rename leaves
such a file ignored by `load()` and `scan()`. A crash after rename leaves a
complete durable record; a later commit returns `already_committed` when the
immutable execution payload matches, without rewriting the first admission
receipt. A different payload for the same deal ticket returns `conflict`.

## Authority boundary

The store accepts a fact only after local structural validation. A successful
`commit()` means that those bytes were durably stored under fencing; it does
not mean that the fact was independently observed in MT5 history or committed
by the journal. Missing or corrupt bytes fail closed. B3B must add durable
source reload and independent verification before any allocation publication.

## Scope exclusions

This ADR does not add `ManagedTradeOwner` wiring, production INOUT, exposure
mutation, terminal lifecycle proof, Trade Event Observation, or public trade
APIs. Those remain separate bounded slices.

# ADR 0003: Host-driven realtime subscriptions (ABI 6)

## Status

Accepted for realtime v1.

## Decision

Expose POD-only subscription requests, generation-qualified handles, and a
borrowed event callback through ABI 6. A request contains one or more
`Mt5TickSourceRequest` entries, making one handle a logical subscription group.
MT5 has no push tick API in the Python package, so the implementation polls
`copy_ticks_from()` in one physical source per `(symbol, flags)`. Group members
have independent sequence cursors while each source owns a bounded ring.

Each poll is split into a lossless forward pagination pass and a separately
bounded overlap reconciliation pass. `max_batch` is the page size, never a
limit on the amount of forward history traversed. This distinction is required
because MT5 may return thousands of records with one timestamp; using the
overlap window as the cursor can otherwise loop forever. A timeout-bearing
non-empty response contributes observable data but leaves the source in
RECONNECTING until a clean confirmation is received.

Delivery is host-driven through `mt5bridge_process_events()`. Callbacks execute
without the runtime mutex and may re-enter the bridge. Event kinds are tick
batches, lifecycle status, and explicit GAP/overflow notifications. No STL,
Python objects, JSON values, or third-party allocator pointers cross the ABI.

Shutdown rejects new work, signals and joins the poller, then performs
MetaTrader and CPython teardown. Realtime v1 delivers raw source batches only.
Coherent cross-symbol snapshots are reserved until watermark and staleness
semantics are implemented; the corresponding delivery flag is rejected rather
than silently approximated. Consumer overruns remain explicit and consumers
use the historical POD API for catch-up.
Source diagnostics are exported per source index. Consumer GAP/overflow state
is kept on the logical member and is not attributed to a shared physical
source. `Mt5GapReason`, `Mt5SnapshotItem`, and `Mt5SnapshotView` reserve the
ABI representation needed for future coherent snapshots without exposing a
partial implementation today.

## Consequences

The API is predictable for GUI/server owner loops and safe for high-volume POD
data, at the cost of requiring the host to call `process_events()` regularly.
The bounded ring makes backpressure visible and prevents unbounded memory
growth. Runtime packaging remains a separate follow-up covered by ADR 0002.

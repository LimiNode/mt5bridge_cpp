# ADR 0003: Host-driven realtime subscriptions (ABI 7)

## Status

Accepted for realtime v1.

## Decision

Expose POD-only subscription requests, generation-qualified handles, and a
borrowed event callback through ABI 7. A request contains one or more
`Mt5TickSourceRequest` entries, making one handle a logical subscription group.
MT5 has no push tick API in the Python package, so the implementation polls
`copy_ticks_from()` in one physical source per `(symbol, flags)`. Group members
have independent sequence cursors while each source owns a bounded ring. A
source keeps a full-payload boundary multiset for its inclusive cursor, because
MT5 may reorder rows that share one `time_msc` between reads. The persistent
boundary contains records for exactly one timestamp (the current cursor);
late inserts from an older timestamp are delivered but never mixed into the
new boundary.

Each poll is split into a lossless forward pagination pass and a separately
bounded overlap reconciliation pass. Both passes use the same non-destructive
full-payload boundary matcher. `max_batch` is the logical delivery batch
size; physical history pages use a separate minimum page size and never limit
the amount of forward history traversed. This distinction is required
because MT5 may return thousands of records with one timestamp; using the
overlap window as the cursor can otherwise loop forever. A timeout-bearing
non-empty response contributes observable data but leaves the source in
RECONNECTING until a clean confirmation is received.
Observed progress is tracked separately from the committed cursor. Upstream
partial epochs are retained for diagnostics but are not published; a clean
replay from the committed cursor delivers each tick once. Forward replay is
deduplicated only by the committed `(time_msc, ordinal)` cursor, never by the
overlap snapshot: observed-but-unpublished records must be delivered after
recovery. Local epoch-budget
exhaustion may continue from the observation. A one-million-tick budget per
forward/reconciliation phase and `max_batch`-sized publication keep catch-up
memory bounded.

Delivery is host-driven through `mt5bridge_process_events()`. Callbacks execute
without the runtime mutex and may re-enter the bridge. Event kinds are tick
batches, lifecycle status, and explicit GAP/overflow notifications. Source
inconsistency GAPs carry a recovery time range; no STL,
Python objects, JSON values, or third-party allocator pointers cross the ABI.

Shutdown rejects new work, signals and joins the poller, then performs
MetaTrader and CPython teardown. Realtime v1 delivers raw source batches only.
Coherent cross-symbol snapshots are reserved until watermark and staleness
semantics are implemented; the corresponding delivery flag is rejected rather
than silently approximated. Consumer overruns remain explicit and consumers
use the historical POD API for catch-up.
The first forward cursor is the source creation timestamp. The overlap window
is used only for reconciliation, so a new subscription does not emit
pre-subscription ticks as realtime data.
Source diagnostics are exported per source index. Consumer GAP/overflow state
is kept on the logical member and is not attributed to a shared physical
source. ABI 7 adds recovery range fields and a tagged snapshot pointer to the
event view. `Mt5SnapshotItem` and `Mt5SnapshotView` reserve the representation
needed for future coherent snapshots without exposing a partial implementation
today; snapshot delivery remains explicitly unsupported.

## Consequences

The API is predictable for GUI/server owner loops and safe for high-volume POD
data, at the cost of requiring the host to call `process_events()` regularly.
The bounded ring makes backpressure visible and prevents unbounded memory
growth. Runtime packaging remains a separate follow-up covered by ADR 0002.

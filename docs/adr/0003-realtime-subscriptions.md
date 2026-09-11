# ADR 0003: Host-driven realtime subscriptions (ABI 6)

## Status

Accepted for realtime v1.

## Decision

Expose POD-only subscription requests, generation-qualified handles, and a
borrowed event callback through ABI 6. MT5 has no push tick API in the Python
package, so the implementation polls `copy_ticks_from()` in one physical
source per `(symbol, flags)`. Logical subscribers have independent sequence
cursors while the source owns a bounded ring of batches.

Delivery is host-driven through `mt5bridge_process_events()`. Callbacks execute
without the runtime mutex and may re-enter the bridge. Event kinds are tick
batches, lifecycle status, and explicit GAP/overflow notifications. No STL,
Python objects, JSON values, or third-party allocator pointers cross the ABI.

Shutdown rejects new work, signals and joins the poller, then performs
MetaTrader and CPython teardown. Realtime v1 does not synthesize bars and does
not hide consumer overruns; consumers use the historical POD API for catch-up.

## Consequences

The API is predictable for GUI/server owner loops and safe for high-volume POD
data, at the cost of requiring the host to call `process_events()` regularly.
The bounded ring makes backpressure visible and prevents unbounded memory
growth. Runtime packaging remains a separate follow-up covered by ADR 0002.

# Market-data API

## Control plane versus data plane

JSON remains the control/RPC plane for initialization, terminal/account
metadata, orders, symbol selection, and rare operations. Ticks and rates use
typed POD buffers so a large numeric result does not make a JSON → UTF-8 → JSON
round trip.

```text
control: initialize / account_info / order_send  -> eval_json()
data:    ticks / rates                           -> POD buffer or chunks
```

The public types live in [include/mt5bridge/data.h](../include/mt5bridge/data.h)
and are C-compatible. `mt5bridge::Client` exposes `copy_ticks_range()` and
`copy_rates_range()` returning ordinary C++ vectors, while the C API exposes
DLL-owned buffers and an optional tick-chunk callback.

Typical C++ usage keeps the POD contract out of application plumbing:

```cpp
Mt5FetchDiagnostics diagnostics{};
auto ticks = mt5.copy_ticks_range("EURUSD", from_msc, to_msc, 0, &diagnostics);
auto rates = mt5.copy_rates_range("EURUSD", /*TIMEFRAME_M1=*/1,
                                  from_msc, to_msc, &diagnostics);
```

Every buffer is owned by the DLL and must be released with its matching
`*_buffer_free()` function. Callback memory is valid only during the callback;
copy it to application-owned storage if it must outlive the call.

## Reliability contract

History reads are retrieved through bounded `copy_ticks_from()` pages and
retried up to three times with bounded backoff when MT5 returns no result
during history warm-up. An empty array is a valid `MT5_FETCH_EMPTY` result; it
is not treated as an error. Diagnostics record attempts, retries, warm-up
detection, and completion status. The callback API delivers each page outside
the Python/GIL critical section, so a consumer does not need to retain a
year-sized NumPy allocation in the DLL.
Callbacks may return non-zero to cancel delivery; they must not re-enter the
bridge while the callback is running.

## Cursor rule for future pagination

Do not advance a cursor with `last_time_msc + 1`: multiple ticks can share one
millisecond. Use `(time_msc, ordinal_at_timestamp)` and request the boundary
timestamp inclusively, discarding exactly the already-consumed ordinal count.
On reconnect, resume from the last committed cursor and tolerate a bounded
overlap before deduplication.

## Safety rules

- Retry reads and synchronization; never blindly retry `order_send` after a
  timeout because the broker may already have accepted the order.
- A partial result must carry diagnostics and must not be reported as complete.
- Keep callback execution outside the Python/GIL critical section.
- Keep allocations bounded by chunk size once paged retrieval is enabled.

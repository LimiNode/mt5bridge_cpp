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
DLL-owned buffers, an optional tick-chunk callback, and
`mt5bridge_last_fetch_diagnostics()` for failures that do not produce a buffer.

`Mt5Tick` preserves both volume representations returned by MT5: integer
`volume` (`uint64_t`) and exchange-provided `volume_real` (`double`). The
runtime reads the structured NumPy buffer directly by field offset and creates
no Python tuple, dictionary, or list per tick.

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

Rates use the same bounded transient-error recovery as ticks. Because
`copy_rates_range()` has no page-size contract, the bridge confirms two stable
successive NumPy results (including two stable empty results) before marking a
rate buffer complete. The transient-failure budget and confirmation-probe
budget are independent, so a clean result received after recovery still gets
its confirmation probe.

## Reliability contract

History reads are retrieved through bounded `copy_ticks_from()` pages and
retried up to three times with bounded backoff when MT5 returns no result or a
classified partial page during history warm-up. Empty and short successful
pages receive bounded confirmation probes before the reader declares the range
complete. Diagnostics record attempts, retries, warm-up detection, and
completion status. The callback API delivers each page outside
the Python/GIL and runtime-mutex critical sections, so a consumer does not need
to retain a year-sized NumPy allocation in the DLL. Callbacks may return
non-zero to cancel delivery and may call another bridge operation; shutting
the bridge down from a callback cancels the outer traversal on its next page.

## Pagination cursor rule

Do not advance a cursor with `last_time_msc + 1`: multiple ticks can share one
millisecond. Use `(time_msc, ordinal_at_timestamp)` and request the boundary
timestamp inclusively, discarding exactly the already-consumed ordinal count.
The next page request grows by that ordinal, so more than 65,536 ticks sharing
one timestamp cannot stall the reader. A page that does not advance either the
timestamp or its consumed ordinal fails instead of looping forever. On IPC
reconnect, traversal resumes from the last committed cursor.

## Realtime subscriptions (ABI 6)

`mt5bridge_copy_ticks_range()` is finite chunked delivery of one historical
range. ABI 6 adds host-driven realtime subscriptions. `mt5bridge_subscribe_ticks()`
accepts an array of `Mt5TickSourceRequest`; one handle may group several
symbols, while equal `(symbol, flags)` requests share one physical
`copy_ticks_from()` polling source. A single symbol is a one-element array and
every source-specific event carries `source_index`; no global chronological
order is promised across symbols. The source uses the smallest
requested interval and retains only a bounded ring of batches. Each poll
re-reads a measured overlap window and reconciles full tick payloads as a
multiset, because MT5 can insert late records, reorder same-time records, or
return identical payloads for distinct events.

The host calls `mt5bridge_process_events(max_events, callback, user)` from its
owner loop. Callback views are borrowed until return and contain `TICK_BATCH`,
`STATUS`, or explicit `GAP` events. A GAP reports batches overrun in the ring;
loss is never silently reported as complete. Consumer overrun is not repaired
automatically in v1; applications needing lossless recovery should issue a
historical POD query from their last committed cursor before resuming.
Handles include a runtime generation and stale handles are rejected. The
contract is best-effort lossless relative to observable synchronized MT5
history; it cannot promise recovery of records that MT5 later rewrites or
removes. `MT5_DELIVERY_COHERENT_SNAPSHOT` is reserved for the next snapshot
phase and is rejected until watermark/staleness semantics are implemented.
Shutdown signals and joins the poller before MetaTrader or CPython teardown.

## NumPy-to-POD benchmark

`tests/benchmark_numpy_to_pod.py` exercises the public DLL ABI with a
deterministic in-process NumPy source. Release x64 results from 2026-09-10 with
Python 3.11.9 and NumPy 2.4.6 were:

| Ticks | Structured array | Pages | Slice baseline | DLL buffer | DLL callback |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 5.7 MiB | 3 | 0.513 ms | 73.253 ms (1.4 M ticks/s) | 58.977 ms (1.7 M ticks/s) |
| 1,000,000 | 57.2 MiB | 17 | 6.652 ms | 191.439 ms (5.2 M ticks/s) | 124.443 ms (8.0 M ticks/s) |
| 5,000,000 | 286.1 MiB | 78 | 30.998 ms | 617.722 ms (8.1 M ticks/s) | 345.027 ms (14.5 M ticks/s) |

The slice number is the fake fetch baseline. DLL columns include Python-call,
pagination, native field-copy conversion, and delivery overhead; they exclude
real terminal IPC and history synchronization latency. Run the script on the
deployment machine before setting a production throughput budget.

The deterministic reliability suite requires a DLL built against the same
Python major/minor version as the test process:

```powershell
ctest --test-dir build -C Release --output-on-failure
$env:MT5BRIDGE_DLL = (Resolve-Path 'build\bin\Release\mt5_bridge.dll').Path
python tests\benchmark_numpy_to_pod.py
```

`ctest --test-dir build -C Release --output-on-failure` also runs a native
owned-interpreter smoke test with a dependency-free fake `MetaTrader5` module.

## Safety rules

- Retry reads and synchronization; never blindly retry `order_send` after a
  timeout because the broker may already have accepted the order.
- A partial result carries diagnostics and is never reported as complete; a
  failed query's snapshot is available through
  `mt5bridge_last_fetch_diagnostics()`.
- Keep callback execution outside the Python/GIL and runtime-mutex sections.
- Prefer callback delivery when the complete range need not be retained; the
  buffer API intentionally accumulates the full result.

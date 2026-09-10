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

## Reliability contract

History reads are retrieved through bounded `copy_ticks_from()` pages and
retried up to three times with bounded backoff when MT5 returns no result
during history warm-up. An empty array is a valid `MT5_FETCH_EMPTY` result; it
is not treated as an error. Diagnostics record attempts, retries, warm-up
detection, and completion status. The callback API delivers each page outside
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

## Realtime boundary

`mt5bridge_copy_ticks_range()` is finite chunked delivery of one historical
range. ABI 3 does not expose `subscribe_ticks()` and must not be described as a
live subscription API. A future realtime layer must persist the committed
`(time_msc, ordinal)` cursor, poll the history tail, reconnect, catch up the
missing interval, suppress only the intentional overlap, and then resume live
delivery. Reading only `symbol_info_tick()` after reconnect is not sufficient.

## NumPy-to-POD benchmark

`tests/benchmark_numpy_to_pod.py` exercises the public DLL ABI with a
deterministic in-process NumPy source. Release x64 results from 2026-09-10 with
Python 3.11.9 and NumPy 2.4.6 were:

| Ticks | Structured array | Pages | Slice baseline | DLL buffer | DLL callback |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 5.7 MiB | 2 | 0.475 ms | 8.416 ms (11.9 M ticks/s) | 5.961 ms (16.8 M ticks/s) |
| 1,000,000 | 57.2 MiB | 16 | 6.532 ms | 124.288 ms (8.0 M ticks/s) | 59.479 ms (16.8 M ticks/s) |
| 5,000,000 | 286.1 MiB | 77 | 30.600 ms | 541.882 ms (9.2 M ticks/s) | 299.147 ms (16.7 M ticks/s) |

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

## Safety rules

- Retry reads and synchronization; never blindly retry `order_send` after a
  timeout because the broker may already have accepted the order.
- A partial result must carry diagnostics and must not be reported as complete.
- Keep callback execution outside the Python/GIL and runtime-mutex sections.
- Prefer callback delivery when the complete range need not be retained; the
  buffer API intentionally accumulates the full result.

# Market-data API

## Control plane versus data plane

JSON remains the control/RPC plane for initialization, terminal metadata, and
legacy read-only operations. Typed trade observations use `trade.h`; ticks and
rates use typed POD buffers so a large numeric result does not make a JSON →
UTF-8 → JSON round trip.

```text
control: initialize / terminal_info / read-only RPC -> eval_json()
trade:   account_info / symbol_info / order_check  -> trade.h POD
data:    ticks / rates                              -> POD buffer or chunks
```

ABI 8 does not expose a side-effecting JSON order helper. The historical
`open_market_buy` method is rejected before reaching MetaTrader; order sending
is reserved for the durable Stage 2 journal.

The public types live in [include/mt5bridge/data.h](../include/mt5bridge/data.h)
and are C-compatible. `mt5bridge::Client` exposes `copy_ticks_range()` and
strict-by-default `copy_rates_range()` returning ordinary C++ vectors. The
explicit `query_rates_range()` method returns best-effort values with
diagnostics and coverage, while the C API exposes
DLL-owned buffers, an optional tick-chunk callback, and
`mt5bridge_last_fetch_diagnostics()` for failures that do not produce a buffer.

`Mt5Tick` preserves both volume representations returned by MT5: integer
`volume` (`uint64_t`) and exchange-provided `volume_real` (`double`). The
runtime reads the structured NumPy buffer directly by field offset and creates
no Python tuple, dictionary, or list per tick.

Typical C++ usage keeps the POD contract out of application plumbing:

```cpp
Mt5FetchDiagnostics diagnostics{};
Mt5RateCoverageV1 coverage{};
auto ticks = mt5.copy_ticks_range("EURUSD", from_msc, to_msc, 0, &diagnostics);
auto rates = mt5.copy_rates_range("EURUSD", /*TIMEFRAME_M1=*/1,
                                  from_msc, to_msc, &diagnostics, &coverage);
auto best_effort = mt5.query_rates_range("EURUSD", /*TIMEFRAME_M1=*/1,
                                         from_msc, to_msc);
```

Every buffer is owned by the DLL and must be released with its matching
`*_buffer_free()` function. Callback memory is valid only during the callback;
copy it to application-owned storage if it must outlive the call.

Rates use the same bounded transient-error recovery as ticks. Because
`copy_rates_range()` has no page-size contract, the bridge confirms two stable
successive NumPy results before returning a rate buffer. Stability alone is not
range completeness: the additive immutable `Mt5RateCoverageV1` extension reports whether
the timeframe-aligned requested boundaries were evidenced. A stable suffix or
stable empty result is returned with `MT5_FETCH_PARTIAL` and
`MT5_RATE_COVERAGE_UNPROVEN`, never as `MT5_FETCH_COMPLETE`. The strict C++
`copy_rates_range()` throws for that partial/unproven result; callers that need
best-effort rows must use `query_rates_range()` and inspect `complete()`.
The C accessor is explicitly versioned as
`mt5bridge_rate_buffer_coverage_v1()`; a future V2 must use a new type and
symbol rather than writing into a V1 caller's buffer.
Rows outside the requested millisecond range are filtered locally before they
cross the POD boundary. Coverage is a boundary claim, not a proof of every
interior bar: the raw response must reach the ceil-aligned first bar and the
filtered response must reach the floor-aligned last bar for a fixed intraday
timeframe. D1/W1/MN1 remain unproven because their calendar/session boundaries
are not fixed Unix periods. The
transient-failure budget and confirmation-probe budget are
independent, so a clean result received after recovery still gets its
confirmation probe.

## Reliability contract

History reads are retrieved through bounded `copy_ticks_from()` pages and
retried up to three times with bounded backoff when MT5 returns no result or a
classified partial page during history warm-up. Empty and short successful
pages receive bounded confirmation probes before the reader declares the range
complete. The confirmation counter resets whenever a page contributes a newly
accepted tick, so only consecutive probes without progress can prove
completion. Diagnostics record attempts, retries, warm-up detection, and
completion status. The callback API delivers each page outside
the Python/GIL and runtime-mutex critical sections, so a consumer does not need
to retain a year-sized NumPy allocation in the DLL. Callbacks may return
non-zero to cancel delivery and may call another bridge operation; shutting
the bridge down from a callback cancels the outer traversal on its next page.
A request spanning at least thirty days first runs a bounded progressive
bootstrap. The bridge issues one-row synchronization probes from a recent
search cursor toward the requested start, initially stepping back 366 days and
halving the step down to a 30-day floor when the oldest observed tick does not
move. At most sixteen probes are issued; three consecutive stalled probes at
the minimum step fail closed with a bootstrap diagnostic. The bootstrap also
has a bounded wall-clock budget and increasing backoff between probes. Every
probe acquires and releases runtime admission and the GIL independently;
backoff never holds either serialization scope. Probe rows are discarded and
are never mixed into the result: the normal inclusive paginator still proves
exact range coverage, boundary multiplicity, and completion. A target-anchor
probe must be preceded by positive tick evidence and followed by a clean
confirmation of the same oldest available tick; merely repeating the current
suffix at the requested start is not success. Clean empty probes only advance
the bounded search cursor and never establish coverage. If all probes are
empty, bootstrap remains retry-exhausted instead of claiming a deep range is
empty. The accepted anchor gap is bounded by the 30-day minimum adaptive step,
so a distant stale suffix remains retry-exhausted instead of claiming a
missing prefix is covered. This permits ordinary weekend/holiday gaps without
turning an unbounded absence of history into evidence.
A successful probe at the requested anchor is only a synchronization request,
not proof that the entire history is present. Warm-up diagnostics are emitted
only after synchronization evidence is observed; a later malformed or other
non-transient response remains `MT5_FETCH_FATAL_ERROR`.
A non-empty page accompanied by a transient history status (including 4403) is
provisional: it is neither delivered nor used to advance the cursor. The next
attempt replays the same inclusive boundary until a clean page arrives or the
bounded partial-page budget is exhausted.

A clean short page whose rows are all already present at the committed boundary
(`last_timestamp == cursor_msc` with no new boundary multiplicity) is a valid
end-of-range observation; it is confirmed with the same bounded probes before
completion. A response whose last timestamp is older than the active cursor is
never accepted as EOF and fails as stale data. In contrast, a full page that
does not advance the timestamp or boundary payload multiset is treated as a
non-progress error so a malformed MT5 response cannot spin the reader
indefinitely.

## Pagination cursor rule

Do not advance a cursor with `last_time_msc + 1`: multiple ticks can share one
millisecond. Keep the boundary timestamp and a multiset of full payloads already
consumed at that timestamp; request the boundary inclusively and discard only
matching payload occurrences. This remains lossless if MT5 changes the order of
same-millisecond rows between pages. The next page request grows by the number
of consumed boundary records, so more than 65,536 ticks sharing one timestamp
cannot stall the reader. A page that does not advance either the timestamp or
the consumed boundary multiset fails instead of looping forever. On IPC
reconnect, traversal resumes from the last committed cursor.

## Realtime subscriptions (ABI 7, preserved by ABI 8)

`mt5bridge_copy_ticks_range()` is finite chunked delivery of one historical
range. ABI 7 adds host-driven realtime subscriptions. `mt5bridge_subscribe_ticks()`
accepts an array of `Mt5TickSourceRequest`; one handle may group several
symbols, while equal `(symbol, flags)` requests share one physical
`copy_ticks_from()` polling source. A single symbol is a one-element array and
every source-specific event carries `source_index`; no global chronological
order is promised across symbols. The source uses the smallest requested
interval and retains only a bounded ring of batches. Each poll epoch has two
separate phases: a forward, lossless page traversal from the committed
`(time_msc, boundary payload multiset)` cursor to the current observation time. `max_batch`
controls delivery batch size only; physical history pages use an independent
minimum page size to avoid quadratic same-timestamp rereads. This is followed
by a bounded tail reread for overlap reconciliation using the same full-payload
boundary matcher. The tail is never used as the forward cursor, so a dense
overlap cannot trap the poller rereading the same first page forever.
Each epoch has a hard one-million-tick safety budget and publishes catch-up in
`max_batch`-sized ring batches. The validated product
`max_batch * ring_capacity` is bounded to one million retained ticks, so the
ring capacity is bounded by both batch count and payload size. If the budget or pagination proof is exhausted, the
source remains `RECONNECTING` instead of claiming `READY`.
Each physical realtime page is one bounded `market_data` admission. The bridge
copies that page into native tick records and releases the interpreter and lane
before doing C++ boundary/multiplicity work or acquiring the next page, so a
long catch-up epoch cannot monopolize the runtime lane. The forward and tail
passes share one `now_msc` snapshot captured at epoch start.
Reconciliation compares complete tick payload multiplicities and reports
rewrites or disappeared records in diagnostics. A non-empty page accompanied
by an MT5 timeout/IPC status is retained for reconciliation but is not published
to consumers; the next forward pass restarts from the last committed cursor and
delivers each tick once after a clean confirmation. Local epoch-budget
exhaustion may continue from the observation.
Each poll captures a `now_msc` snapshot before entering MT5 IPC. Because
`copy_ticks_from()` has no upper time bound, a response may contain rows that
arrived after that snapshot. Such rows are ignored for the current epoch and
never advance the cursor; the last accepted row at or before `now_msc` remains
the boundary, so accepted ticks cannot be repeated on the next poll.
The first forward poll starts at the source creation timestamp; the overlap
window is reserved for reconciliation and does not turn pre-subscription ticks
into realtime events.
For compatibility, `delivery_flags == 0` is defined as the default
`MT5_DELIVERY_TICK_BATCH` mode.

The host calls `mt5bridge_process_events(max_events, callback, user)` from its
owner loop. Callback views are borrowed until return and contain `TICK_BATCH`,
`STATUS`, or explicit `GAP` events. A GAP reports batches overrun in the ring;
loss is never silently reported as complete. Consumer overrun is not repaired
automatically in v1; applications needing lossless recovery should issue a
historical POD query from their last committed cursor before resuming. Use
`mt5bridge_subscription_source_diagnostics()` to inspect a particular member
of a grouped subscription; `mt5bridge_subscription_diagnostics()` is a
compatibility shorthand for source index zero. Consumer overflow is reported
on the GAP event (`gap_reason == MT5_GAP_CONSUMER_OVERFLOW`) and is intentionally
not written into shared physical-source diagnostics.
Event sequence numbers are monotonic within a source for TICK_BATCH and GAP
events, including inconsistency GAPs; a GAP uses the next sequence that can be
produced, never the oldest retained ring entry. STATUS events have
`sequence == 0` and are outside the data sequence.
`MT5_SUBSCRIPTION_STOPPED` is reserved in ABI 7 and is not emitted by v1;
shutdown invalidates the handle after joining the poller.
The callback return value is a cancellation signal: a non-zero return consumes
the current event and stops the call. Consequently the function's return count
includes that event even though no later events are delivered.
Handles include a runtime generation and stale handles are rejected. The
contract is best-effort lossless relative to observable synchronized MT5
history; it cannot promise recovery of records that MT5 later rewrites or
removes. `MT5_DELIVERY_COHERENT_SNAPSHOT` is reserved for the next snapshot
phase and is rejected until watermark/staleness semantics are implemented. The
ABI 7 reserves a tagged snapshot pointer in `Mt5SubscriptionEvent`, together
with `Mt5SnapshotItem` and `Mt5SnapshotView`, for that extension.
Until snapshot delivery is implemented, non-zero `stale_after_ms` is rejected
instead of silently ignored.
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

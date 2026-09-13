# MetaTrader 5 quirks and recovery references

This is a searchable registry, not a promise that every forum workaround is
universally correct. Each entry records the symptom, safe bridge behavior, and
the upstream reference to re-check when MT5 or the Python package changes.

| Tag | Symptom | Bridge rule | Reference |
| --- | --- | --- | --- |
| `history-warmup` | First history request returns no/partial data while the terminal synchronizes its local series | Retry bounded history reads; distinguish empty history from unavailable history; expose diagnostics | [CopyTicks](https://www.mql5.com/en/docs/series/copyticks), [MQL5 forum 393357/page69](https://www.mql5.com/en/forum/393357/page69) |
| `history-timeout` | Synchronization can time out or return a partial result, including MQL error 4403 | Mark the result partial/retry-exhausted; never infer completeness from `size > 0` | [MQL5 error codes](https://www.mql5.com/ru/docs/constants/errorswarnings/errorcodes), [CopyTicksRange](https://www.mql5.com/ru/docs/series/copyticksrange) |
| `ipc-transport` | Python API reports `RES_E_INTERNAL_FAIL_*` (`-10000` through `-10005`) or returns partial data with an IPC status | Preserve committed data, call `shutdown()`/`initialize()`, and retry only idempotent reads from the inclusive cursor; expose reconnect counters | [Python last_error](https://www.mql5.com/en/docs/python_metatrader5/mt5lasterror_py), [MQL5 forum Python integration](https://www.mql5.com/en/forum/393357) |
| `series-sync` | Native series APIs expose an explicit synchronization state | Model warm-up as a state machine; do not retry every empty result forever | [Bars / series synchronization](https://www.mql5.com/en/docs/series/bars) |
| `python-history` | Python `copy_*` calls return `None` on failures and NumPy arrays on success | Convert `None` to classified bridge errors; copy numeric fields from the array buffer | [Python Integration](https://www.mql5.com/en/docs/python_metatrader5), [copy_ticks_from](https://www.mql5.com/en/docs/python_metatrader5/mt5copyticksfrom_py) |
| `side-effect-timeout` | `order_send()` can time out after the server accepted the order | Never auto-retry without reconciliation/idempotency | [Python order_send](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersend_py) |
| `tick-timestamp-ties` | Several ticks may share the same `time_msc` | Use an inclusive cursor and a full-payload boundary multiset; never use `time_msc + 1` pagination | [CopyTicksRange](https://www.mql5.com/ru/docs/series/copyticksrange) |
| `same-ms-order-instability` | MT5 may return equal-`time_msc` ticks in a different order between inclusive reads | Match consumed boundary payload multiplicities instead of skipping positional rows; regression-test rotated buckets | [MQL5 forum 396877/page42](https://www.mql5.com/en/forum/396877/page42), [CopyTicks unexpected results](https://www.mql5.com/en/forum/370322) |
| `copyticks-before-from` | `CopyTicks` may include rows older than the requested `from` boundary | Filter rows before the requested boundary and keep a regression for inclusive pagination | [MQL5 forum 413688/page27](https://www.mql5.com/en/forum/413688/page27) |
| `poll-loss` | `symbol_info_tick()` returns only the current tick, so polling can miss updates | Use continuous history catch-up with `copy_ticks_from()` for reliable streams | [Python Integration](https://www.mql5.com/en/docs/python_metatrader5) |
| `independent-stream-order` | INFO and TRADE streams may be inserted into history out of timestamp order; a later-observed tick can have an earlier `time_msc` | Re-read a bounded overlap and reconcile by full-payload multiplicity; never treat the latest timestamp as a permanent high-water mark | [MQL5 forum 396877/page19](https://www.mql5.com/en/forum/396877/page19), [MQL5 forum 42122/page20](https://www.mql5.com/ru/forum/42122/page20) |
| `copyticks-lag` | `CopyTicks`/history synchronization can lag behind the live terminal and may take hundreds of milliseconds or longer | Keep periodic polling even when a cheap head hint is unchanged; use adaptive overlap and expose history lag | [MQL5 forum 342090/page57](https://www.mql5.com/ru/forum/342090/page57) |
| `timeout-with-data` | A non-empty `CopyTicks` result can still carry `ERR_HISTORY_TIMEOUT`/synchronization status | Inspect result and `last_error` together; retain data for reconciliation but do not emit a stable READY/caught-up state until a later confirmation succeeds | [CopyTicks](https://www.mql5.com/en/docs/matrix/matrix_initialization/matrix_copyticks), [MQL5 forum 396877/page43](https://www.mql5.com/en/forum/396877/page43) |
| `history-rewrite` | A running or freshly restarted terminal can expose different history for the same interval | Promise only best-effort lossless delivery relative to observable synchronized history; detect and report rewrites instead of claiming broker-level guarantees | [MQL5 forum 396877/page46](https://www.mql5.com/en/forum/396877/page46) |
| `ontick-coalescing` | `OnTick` is a market-state notification: a new event is not queued while another NewTick is pending/processing | Do not use an MQL `OnTick` bridge as an exchange-tick transport; use CopyTicks history reconciliation | [OnTick](https://www.mql5.com/en/docs/event_handlers/ontick), [OnTickMulti discussion](https://www.mql5.com/ru/forum/458908) |
| `multisymbol-view-not-atomic` | A multisymbol `OnTick`/state view can expose different symbols at different processing points | Keep raw grouped subscriptions non-coherent; reject snapshot delivery until common watermark/staleness semantics exist | [OnTickMulti discussion](https://www.mql5.com/ru/forum/458908), [MQL5 forum 370322](https://www.mql5.com/en/forum/370322) |
| `access-point-switch` | Automatic access-point changes can stall history or return `4403` until synchronization recovers | Enter RECONNECTING, apply bounded exponential backoff, verify connectivity, then reconcile an enlarged overlap | [MQL5 forum 42122/page49](https://www.mql5.com/ru/forum/42122/page49), [CopyTicks](https://www.mql5.com/ru/docs/series/copyticks) |
| `copyticks-latency` | CopyTicks initiates server synchronization and can block for a long time on poor connectivity | Never hammer the API at a fixed 10–20 ms cadence during recovery; use retry deadlines/backoff and publish diagnostics | [CopyTicks](https://www.mql5.com/ru/docs/series/copyticks) |
| `trade-reconciliation` | After a trading call, orders/deals/positions may become mutually consistent only after a delay | Never blindly retry `order_send`; expose raw result and add a separate bounded confirmation/reconciliation phase | [MT4Orders discussion](https://www.mql5.com/zh/forum/164943/page64), [order_send](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersend_py) |
| `cpython-restart` | Binary extension modules may retain process-global state across `Py_FinalizeEx()` | Do not promise restart-in-process until a 100-cycle packaged-runtime/live-terminal acceptance test passes; prefer a worker process if unstable | [CPython initialization/finalization](https://docs.python.org/3/c-api/init.html) |

## Realtime interpretation rule

Realtime delivery is **best-effort lossless relative to the observable,
synchronized MT5 history**. It is not a broker-level guarantee: MT5 may insert,
reorder, delay, or rewrite history after a tick was observed. The source must
therefore poll an overlap window, reconcile complete tick payloads with
multiplicity, keep explicit partial/reconnecting diagnostics, and expose
consumer overflow separately from source/history inconsistency.

## Search playbook

Use these terms when investigating a new symptom:

```text
site:mql5.com/en/forum MetaTrader5 copy_ticks first request empty
site:mql5.com/en/forum MetaTrader5 history synchronization timeout Python
site:mql5.com/en/forum MetaTrader5 copy_rates None
site:mql5.com/en/docs/python_metatrader5 copy_ticks_from last_error
```

When a workaround is confirmed, add a row with the observed terminal build,
Python package version, reproduction, bounded recovery rule, and link. Keep the
implementation in the bridge reliability layer rather than copying a version
check into every consumer.

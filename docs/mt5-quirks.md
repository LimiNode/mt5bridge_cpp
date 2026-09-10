# MetaTrader 5 quirks and recovery references

This is a searchable registry, not a promise that every forum workaround is
universally correct. Each entry records the symptom, safe bridge behavior, and
the upstream reference to re-check when MT5 or the Python package changes.

| Tag | Symptom | Bridge rule | Reference |
| --- | --- | --- | --- |
| `history-warmup` | First history request returns no/partial data while the terminal synchronizes its local series | Retry bounded history reads; distinguish empty history from unavailable history; expose diagnostics | [CopyTicks](https://www.mql5.com/en/docs/series/copyticks), [MQL5 forum 393357/page69](https://www.mql5.com/en/forum/393357/page69) |
| `history-timeout` | Synchronization can time out or return a partial result | Mark the result partial/retry-exhausted; never infer completeness from `size > 0` | [MQL5 error codes](https://www.mql5.com/ru/docs/constants/errorswarnings/errorcodes), [CopyTicksRange](https://www.mql5.com/ru/docs/series/copyticksrange) |
| `series-sync` | Native series APIs expose an explicit synchronization state | Model warm-up as a state machine; do not retry every empty result forever | [Bars / series synchronization](https://www.mql5.com/en/docs/series/bars) |
| `python-history` | Python `copy_*` calls return `None` on failures and NumPy arrays on success | Convert `None` to classified bridge errors; copy numeric fields from the array buffer | [Python Integration](https://www.mql5.com/en/docs/python_metatrader5), [copy_ticks_from](https://www.mql5.com/en/docs/python_metatrader5/mt5copyticksfrom_py) |
| `side-effect-timeout` | `order_send()` can time out after the server accepted the order | Never auto-retry without reconciliation/idempotency | [Python order_send](https://www.mql5.com/en/docs/python_metatrader5/mt5ordersend_py) |
| `tick-timestamp-ties` | Several ticks may share the same `time_msc` | Use an ordinal cursor and inclusive overlap; never use `time_msc + 1` pagination | [CopyTicksRange](https://www.mql5.com/ru/docs/series/copyticksrange) |
| `poll-loss` | `symbol_info_tick()` returns only the current tick, so polling can miss updates | Use continuous history catch-up with `copy_ticks_from()` for reliable streams | [Python Integration](https://www.mql5.com/en/docs/python_metatrader5) |

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

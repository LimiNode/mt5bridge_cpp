# Native implementation guide

The source is split by responsibility:

- `src/bridge/` — exported C ABI adapters and legacy control-plane entry points;
- `src/runtime/` — interpreter lifecycle, runtime admission, and scheduling;
- `src/market/` — market-data history and realtime implementations;
- `src/trade/` — typed trade observation and broker dispatch adapters;
- `src/dispatch/` — durable journal and one-shot dispatch orchestration.

`mt5_bridge` is the only target that may include `Python.h` or link
`Python3::Python`; the `mt5bridge::client` target must remain runtime-agnostic.

## Source layout

Public SDK headers live under `include/mt5bridge/` (with the root
`include/mt5bridge.hpp` umbrella). All source headers are private and remain
beside their implementation in the responsibility directory; do not add a
second private include tree. The bridge adapter may include private seams from
runtime, trade, dispatch, and market through target-local include paths.

The large bridge translation unit is being reduced in responsibility slices;
new market, trade, or dispatch code must not be added to `src/bridge/`.

Keep `g_mutex` for lifecycle/state transitions and use the separate
`g_python_mutex` for serialized interpreter calls. Python/MT5 operations must
enter through the runtime-call admission gate: check `RuntimeState` and
increment the in-flight count under `g_mutex`, release it, and hold only the
admission count plus `g_python_mutex` while invoking Python. Never hold either
mutex while invoking user callbacks or while joining the poller. Acquire the
GIL only while calling Python and release it before returning to native code.
Shutdown closes new admission and waits for in-flight calls before
finalization.
`initialize()` and `shutdown()` are serialized state-machine operations;
`shutting_down` rejects concurrent re-entry. A failed initialization must
finalize the interpreter and leave the bridge ready for a later retry.

New methods should add one short dispatch branch plus focused validation. If
three methods need the same conversion or validation, extract one helper; do
not clone a complete request pipeline. Realtime source/subscription registries
and their poller are intentionally process-global because the DLL owns one
interpreter; keep them behind `g_mutex` and do not add a second runtime state
store.

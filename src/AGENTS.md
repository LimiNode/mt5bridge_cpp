# Runtime implementation guide

The source is split by responsibility, with the heavy implementation under
`src/runtime/`:

1. lifecycle — one Python interpreter, explicit initialize/shutdown;
2. ABI adapter — UTF-8 request/response buffers and error reporting;
3. dispatcher — validated method names and MetaTrader5 calls;
4. conversion — only the small set of Python containers returned by MT5.

`mt5_bridge` is the only target that may include `Python.h` or link
`Python3::Python`; the `mt5bridge::client` target must remain runtime-agnostic.

## Source layout

Public SDK headers live under `include/mt5bridge/` (with the root
`include/mt5bridge.hpp` umbrella). Runtime implementation files live under
`src/runtime/` and are private to the DLL. When a private seam is needed, keep
its `.hpp` and `.cpp` beside each other in `src/runtime/`; do not add a second
private include tree. Leave `mt5_bridge.cpp` as one unit until a real
responsibility boundary justifies extracting a pair.

Keep `g_mutex` for lifecycle/state transitions and use the separate
`g_python_mutex` for serialized interpreter calls. Never hold either mutex
while invoking user callbacks or while joining the poller. Acquire the GIL
only while calling Python and release it before returning to native code.
`initialize()` and `shutdown()` are serialized state-machine operations;
`shutting_down` rejects concurrent re-entry. A failed initialization must
finalize the interpreter and leave the bridge ready for a later retry.

New methods should add one short dispatch branch plus focused validation. If
three methods need the same conversion or validation, extract one helper; do
not clone a complete request pipeline. Realtime source/subscription registries
and their poller are intentionally process-global because the DLL owns one
interpreter; keep them behind `g_mutex` and do not add a second runtime state
store.

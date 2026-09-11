# Runtime implementation guide

The source is split by responsibility, with the heavy implementation under
`src/runtime/`:

1. lifecycle — one Python interpreter, explicit initialize/shutdown;
2. ABI adapter — UTF-8 request/response buffers and error reporting;
3. dispatcher — validated method names and MetaTrader5 calls;
4. conversion — only the small set of Python containers returned by MT5.

`mt5_bridge` is the only target that may include `Python.h` or link
`Python3::Python`; the `mt5bridge::client` target must remain runtime-agnostic.

Keep the lifecycle mutex around the entire Python operation. Acquire the GIL
only while calling Python and release it before returning to native code. A
failed initialization must finalize the interpreter and leave the bridge ready
for a later retry.

New methods should add one short dispatch branch plus focused validation. If
three methods need the same conversion or validation, extract one helper; do
not clone a complete request pipeline. Avoid global mutable state except the
interpreter state, mutex, and thread-local error string.

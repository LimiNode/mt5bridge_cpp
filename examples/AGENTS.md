# Example guide

Examples demonstrate the consumer contract, not private implementation.
Prefer the stable umbrella `<mt5bridge.hpp>` to load `mt5_bridge.dll`; use
`<mt5bridge/client.hpp>` only when an example intentionally demonstrates the
facade in isolation. Do not duplicate
`LoadLibraryW`/`GetProcAddress` blocks or link against implementation internals.
`examples/mt5bridge_loader.hpp` is retained only as a compatibility include.

Every successful `eval_json` call must release its response with
`free_response`, and every initialized bridge must be shut down before the
loader is destroyed. Keep examples runnable without a live terminal where the
scenario allows it.

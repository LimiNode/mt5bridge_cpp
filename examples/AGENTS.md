# Example guide

Examples demonstrate the consumer contract, not private implementation.
Always use `<mt5bridge/client.hpp>` to load `mt5_bridge.dll`; do not duplicate
`LoadLibraryW`/`GetProcAddress` blocks or link against implementation internals.
`examples/mt5bridge_loader.hpp` is retained only as a compatibility include.

Every successful `eval_json` call must release its response with
`free_response`, and every initialized bridge must be shut down before the
loader is destroyed. Keep examples runnable without a live terminal where the
scenario allows it.

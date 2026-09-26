# Public ABI guide

`mt5bridge.hpp` is the recommended public C++ umbrella. `abi.h` is the core
plain-C ABI, `client.hpp` is the lightweight C++ consumer facade, and the
domain umbrellas are `market.h`/`market.hpp`, `trade.h`/`trade.hpp`,
`reconciliation.hpp`, and `dispatch.hpp`. Focused contracts live only under
`market/`, `trade/`, `reconciliation/`, and `dispatch/`. All focused headers
remain self-contained and none pulls in the heavy runtime.

- Keep `extern "C"` exports plain: pointers, integers, and UTF-8 strings.
- Do not add C++ containers, exceptions, `PyObject*`, `json_t*`, or ownership
  that depends on the caller's CRT.
- Returned text is owned by the DLL until `mt5bridge_free`; document ownership
  beside every new pointer result.
- `mt5bridge_last_error()` returns thread-local diagnostic text that remains
  valid until the next bridge call on that thread. It may return `nullptr`.
- Preserve the Windows x64 guard and the `MT5BRIDGE_API` import/export macro.
- `client.hpp` may use WinAPI and the C++ standard library, but must not include
  `Python.h`, Jansson, or link to `Python3::Python`.
- Plain-C domain headers use `.h`; C++ contracts use `.hpp`. Root-level files
  are umbrellas or core entry points, not forwarding aliases for leaf headers.
- `market/*.h` contains only POD requests/results, opaque buffer handles, and
  C callbacks. Keep it free of STL, exceptions, and Python/JSON dependencies.
- `trade/*.h` contains typed account, symbol, order-check, order, position, and
  deal PODs. Use `known_fields` masks to distinguish an absent MT5 field from
  a valid zero; never infer trading permissions from a related enum.

When the ABI changes, update the C++ examples, `python/mt5bridge_py.py`, and
`docs/architecture.md` in the same change.

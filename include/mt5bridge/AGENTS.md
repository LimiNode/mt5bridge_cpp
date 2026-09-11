# Public ABI guide

`abi.h` is the plain-C ABI and `client.hpp` is the lightweight C++ consumer
facade. Both are installed entry points; neither pulls in the heavy runtime.

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
- `data.h` contains only POD requests/results, opaque buffer handles, and C
  callbacks. Keep it free of STL, exceptions, and Python/JSON dependencies.

When the ABI changes, update the C++ examples, `python/mt5bridge_py.py`, and
`docs/architecture.md` in the same change.

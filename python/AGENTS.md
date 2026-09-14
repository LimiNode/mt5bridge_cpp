# Python binding guide

`mt5bridge_py.py` is a thin ctypes adapter. It must mirror the exported C ABI,
not reimplement dispatch or lifetime rules.

The DLL embeds CPython and therefore must be built and loaded with the same
Python major/minor runtime. The C ABI is stable across C++ consumers, not across
arbitrary CPython versions.

- Use `c_void_p` for DLL-owned response pointers.
- Copy response bytes into a Python `str`, then call `mt5bridge_free` in a
  `finally` block.
- Convert non-zero return codes into `RuntimeError` using `last_error()`.
- Keep the DLL name and exported symbols identical to the C++ examples.
- Do not retain pointers returned by the DLL after the call returns.
- Do not add a convenience wrapper that invokes `order_send`; ABI 8 keeps
  trade observations advisory until the durable Stage 2 journal exists.

# Runtime backend guide

This directory contains the shared CPython lifecycle and runtime-admission
implementation used by `mt5_bridge.dll`. It is the only project area allowed
to own interpreter lifecycle and runtime scheduling. Other Python-backed
adapters live in their responsibility directories but are compiled only into
the `mt5_bridge` target.

Keep runtime details behind `include/mt5bridge/abi.h`. The lightweight client
must remain usable with `MT5BRIDGE_BUILD_RUNTIME=OFF`.

Private runtime headers belong next to their implementation files in this
directory. Do not move them into a parallel `include/` tree. Nothing in this
directory is a consumer SDK header.

`python_ref.hpp` is the shared move-only ownership primitive for strong
`PyObject*` references. Keep it narrow; field conversion and domain-specific
Python adapters remain in their owning market or trade implementation.

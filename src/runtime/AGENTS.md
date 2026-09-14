# Runtime backend guide

This directory contains the heavy CPython-backed implementation of
`mt5_bridge.dll`. It is the only project area allowed to include `Python.h` or
depend on `Python3::Python`.

Keep runtime details behind `include/mt5bridge/abi.h`. The lightweight client
must remain usable with `MT5BRIDGE_BUILD_RUNTIME=OFF`.

Private runtime headers belong next to their implementation files in this
directory. Do not move them into a parallel `include/` tree, and do not split
`mt5_bridge.cpp` until a concrete responsibility boundary requires a separate
`.hpp`/`.cpp` pair. Nothing in this directory is a consumer SDK header.

# Reliability test guide

Tests in this directory exercise the exported ABI against deterministic fake
MetaTrader Python modules. Keep them independent of a live terminal and make
the DLL path explicit through `MT5BRIDGE_DLL`. The native owned-runtime smoke
test must keep its fake module free of third-party imports so it validates the
`Py_Initialize`/`Py_FinalizeEx` path independently from NumPy.

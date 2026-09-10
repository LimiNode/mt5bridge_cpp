# Reliability test guide

Tests in this directory exercise the exported ABI against deterministic fake
MetaTrader Python modules. Keep them independent of a live terminal and make
the DLL path explicit through `MT5BRIDGE_DLL`.

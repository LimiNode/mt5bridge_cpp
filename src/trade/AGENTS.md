# Trade implementation guide

This directory owns typed trade observation conversion and the private broker
transport adapter. Python-backed files are compiled only into `mt5_bridge`;
they must use the shared runtime admission and GIL ownership rules.

Keep dispatch orchestration in `src/dispatch/` and interpreter lifecycle in
`src/runtime/`. Private headers remain beside their implementation files.

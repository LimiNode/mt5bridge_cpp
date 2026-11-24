# Trade implementation guide

This directory owns typed trade observation conversion and the private broker
transport adapter. Python-backed files are compiled only into `mt5_bridge`;
they must use the shared runtime admission and GIL ownership rules.

Keep dispatch orchestration in `src/dispatch/` and interpreter lifecycle in
`src/runtime/`. Private headers remain beside their implementation files.

The managed OPEN capability collector takes only identity and a concrete
Windows lease. Its strict Python reader creates plain data; only the bridge's
runtime-admitted wrapper constructs the immutable source snapshot. Never add
caller-POD/module/normalization restoration as an authority path. Collection
success is not OPEN admission: any existing symbol position/order blocks a new
unencumbered OPEN, and age/runtime-generation/fencing checks do not exclude
external trading. Keep this seam disconnected from manager/dispatch until its
later immutable binding and pre-barrier revalidation are independently reviewed.

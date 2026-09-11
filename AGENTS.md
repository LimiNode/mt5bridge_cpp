# AGENTS.md

This is the routing document for AI coding agents working in `mt5bridge_cpp`.
Read the child guide that owns the files you change; keep repo-wide rules here
so every agent gets the same small contract.

## Project boundary

`mt5_bridge.dll` is a Windows x64 C++17 DLL that embeds CPython and forwards a
small, JSON-over-C-ABI request surface to MetaTrader5. The DLL owns Python
lifetime, the GIL, request serialization, and response buffers. Applications
may load it dynamically; they must not depend on private C++ or Python objects.

## Required workflow

1. Inspect `git status --short` before editing and preserve unrelated changes.
2. For code discovery, use the codebase-memory graph first (`index_status`,
   `search_graph`, `trace_path`, `get_code_snippet`), then targeted reads.
3. Keep a change single-purpose. Remove duplication when touching a path, but
   do not introduce a framework or speculative abstraction.
4. Update the relevant guide and both README languages when public behavior
   changes (a Russian README is added when the project adopts one).
5. Configure/build the focused target, or record why the local toolchain cannot
   run it. Do not commit build output, Python runtimes, DLLs, or model files.

## Non-negotiable design rules

- Public DLL functions use C linkage and fixed-width/plain C types only.
- `mt5bridge::Client` is the production C++ consumer facade; examples are not
  its owner and must include it from `include/`.
- Memory crossing the DLL boundary is released by a matching DLL function.
- Never expose STL, `PyObject*`, `json_t*`, or a third-party allocator in the
  public ABI.
- Serialize calls that touch the embedded interpreter; initialization,
  evaluation, and shutdown have deterministic ownership and ordering.
- Keep transport/ABI, Python runtime, request dispatch, and domain operations
  separate. New MetaTrader methods belong in a narrow dispatcher, not in a
  second bridge implementation.
- Prefer one general helper over repeated validation/serialization blocks.
  Delete unreachable branches, redundant checks, and comments that only repeat
  the next line.
- Do not add `noexcept` unless the complete implementation is non-throwing.
- Follow the `log-it-cpp` Doxygen style: every source file has `\file` and
  `\brief`; classes, structs, enums, named constants, methods, and functions
  have concise `\brief` documentation plus `\param`, `\return`, `\note`, or
  `\warning` where those details affect correct use.

## Guide index

- [include/mt5bridge/AGENTS.md](include/mt5bridge/AGENTS.md) — public ABI and
  compatibility rules.
- [src/AGENTS.md](src/AGENTS.md) — runtime lifecycle and implementation layers.
- [src/runtime/AGENTS.md](src/runtime/AGENTS.md) — heavy CPython runtime rules.
- [examples/AGENTS.md](examples/AGENTS.md) — dynamic DLL loading examples.
- [python/AGENTS.md](python/AGENTS.md) — ctypes ownership and Python surface.
- [docs/AGENTS.md](docs/AGENTS.md) — architecture decisions and research notes.

## Review checklist

- Does the change preserve the exported symbol names and calling convention?
- Does the consumer validate `MT5BRIDGE_ABI_VERSION` before invoking a DLL?
- Is every response buffer freed through `mt5bridge_free` exactly once?
- Are all Python API calls protected by the lifecycle mutex and GIL?
- Are errors actionable and safe to read until the next call on the same thread?
- Is the implementation shorter or clearer than a copy-pasted alternative?
- Are focused compile/smoke checks documented?

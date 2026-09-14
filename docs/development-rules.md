# Rules against bloated C++

These rules adapt the PVS-Studio article «Опухший C++ код» (19 Aug 2026) to
this bridge. The article's central observation is that AI-generated repetition,
dead branches, and decorative complexity make review, analysis, compilation,
and future changes more expensive without adding behavior.

## Before adding code

- Search for an existing owner of the behavior. Extend it instead of creating
  a parallel `*_v2`, helper, or near-identical dispatcher.
- State the invariant that requires a new branch. If no input can reach it,
  delete it.
- Prefer the smallest representation that crosses a boundary. Here that means
  UTF-8 JSON and plain C types, not a second serialization library.

## During implementation

- One validation helper owns each repeated rule (string, integer, numeric
  range). One conversion helper owns each repeated Python-container mapping.
- Keep conditions reachable and non-contradictory. Do not check a fact already
  guaranteed by the surrounding branch.
- Do not add `noexcept` for appearance. A function marked `noexcept` must not
  allocate, call user code, or allow an exception to escape.
- Prefer RAII and early returns over duplicated cleanup labels. Keep comments for
  ownership, invariants, and non-obvious compatibility constraints only.
- Do not generate speculative layers, adapters, or configuration options. Add
  an abstraction only when at least two real callers share a stable contract.
- Keep public SDK headers under `include/mt5bridge/` and private runtime
  implementation under `src/runtime/`. Private headers belong beside their
  implementation files; do not create a parallel private include tree.
- Do not split a single implementation unit for visual symmetry. Extract a
  private `.hpp`/`.cpp` pair only when a real responsibility boundary or a
  second caller requires it.

## Review gates

- Compare changed code with nearby methods for copy-paste blocks.
- Run compiler warnings and a static analyzer when available; treat
  always-true/false conditions as cleanup work, not harmless noise.
- Review the diff for removed as well as added lines. A feature that adds a
  second pipeline must justify its maintenance and test cost.
- Keep tests focused on behavior and shared fixtures; do not copy a complete
  setup sequence into every test.

The goal is not a particular line count. The goal is one source of truth per
rule, a short path from request to result, and code whose complexity reflects
real product behavior.

## Source

- PVS-Studio, [«Опухший C++ код»](https://habr.com/ru/companies/pvs-studio/articles/1072264/),
  published 2026-08-19 and reviewed for this project on 2026-09-09.

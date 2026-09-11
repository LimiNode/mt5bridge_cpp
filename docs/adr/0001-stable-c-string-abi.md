# ADR-0001: Stable C ABI for the bridge DLL

- Status: accepted
- Date: 2026-09-09

## Context

The previous public function returned `json_t*`. That coupled every consumer to
Jansson headers, allocator behavior, and a compatible CRT. The Python binding
already expected a different, nonexistent `mt5bridge_eval_json` export.

## Decision

Expose `mt5bridge_abi_version()`, `mt5bridge_eval_json(const char*, char**)`,
and `mt5bridge_free(char*)` as the control plane. Bulk ticks and rates use the
versioned POD declarations in `data.h`, with DLL-owned opaque buffers and a
bounded tick callback. Responses and buffers are allocated and released by the
DLL. Keep all implementation libraries and Python objects private. The
header-only `mt5bridge::Client` validates the version before resolving calls.

The original control/data contract was ABI version 5. ABI 7 adds the realtime
subscription records and exports. Any incompatible change to exported
signatures or POD layout requires a version bump and a coordinated client
update.

ABI 4 separated integer tick `volume` from `volume_real`, made request padding
explicit, fixed public POD sizes and offsets with compile-time assertions, and
uses a fixed-width `Mt5FetchStatus` instead of an implementation-defined C enum.
ABI 5 makes shutdown status-bearing so C and ctypes consumers can detect an
owner-thread or finalization failure before unloading the DLL.

## Consequences

The C++ and ctypes consumers share one contract and can load the DLL without a
Jansson installation. Existing callers using `json_t*` must migrate by dumping
their request to UTF-8 and parsing the returned string in their own process.
The explicit free function adds one call but removes cross-module ownership
ambiguity.

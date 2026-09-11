# ADR-0002: Self-contained runtime distribution for the bridge DLL

- Status: accepted, implementation deferred until the realtime ABI is stable
- Date: 2026-09-11

## Context

`mt5_bridge.dll` embeds CPython and imports the native `MetaTrader5` and NumPy
extensions. A development checkout can use the project virtual environment,
but a distributable application should not require users to install a matching
Python version, wheels, or a particular `PATH` layout. The public ABI is also
expected to change when realtime subscriptions are added, so packaging must
not freeze the current ABI prematurely.

The native `.pyd` files used by NumPy and MetaTrader5 are Windows PE modules.
They rely on the normal Windows loader and may load additional DLLs from their
runtime directories. A true in-memory loader would duplicate relocations,
imports, TLS, exception registration, dependency lookup, and extension-module
loading. That complexity is outside the bridge's domain and would make
diagnostics and security harder.

## Decision

Release the runtime as a self-extracting `mt5_bridge.dll` from the consumer's
point of view, while extracting the embedded runtime payload to a private,
content-addressed directory before CPython initialization:

```text
mt5_bridge.dll
    embedded compressed payload
        -> %LOCALAPPDATA%\\LimiNode\\mt5bridge\\runtime\\<payload-hash>\\
             python311.dll
             python311.zip / Lib/
             site-packages/NumPy/
             site-packages/MetaTrader5/
             dependent runtime DLLs
```

Extraction is performed by `mt5bridge_initialize()`, never from `DllMain`.
The implementation must:

1. derive the payload directory from a fixed payload hash, not from a mutable
   global or the current working directory;
2. verify the embedded payload hash (and, for published builds, its release
   signature) before loading it;
3. use a named process lock and a staging directory followed by an atomic
   rename, so two processes cannot observe a partial runtime;
4. configure `PyConfig.home` and module search paths to the extracted runtime;
5. use restricted DLL search directories (`AddDllDirectory` /
   `LOAD_LIBRARY_SEARCH_*`) rather than globally prepending the payload to
   `PATH`;
6. retain extracted versions for concurrent/rollback use and clean them only
   through a separate, safe cache-retention policy;
7. report extraction, hash, Python, NumPy, and MetaTrader5 failures through the
   existing actionable diagnostics surface.

The first implementation should package a proper Windows embeddable Python
runtime, not the developer `venv` directory. The bundle must include the
standard library, `python311.dll`, the pinned native wheels, and any required
Microsoft runtime or an explicit VC Redistributable prerequisite.

This decision does not mean that all runtime bytes execute directly from the
DLL image. The supported user experience is one bridge DLL plus the host
application; the extracted cache is an implementation detail.

## Consequences

Consumers can deploy one bridge DLL without managing Python installation or
site-packages. Content-addressed directories allow multiple bridge releases to
coexist and make rollback possible. First initialization performs bounded disk
I/O and needs write access to the per-user application-data directory.

The payload increases DLL size and release complexity. Every embedded component
must be reviewed for redistribution terms, especially the MetaTrader5 wheel,
and the release process must publish hashes and licenses. A normal runtime ZIP
remains the diagnostic fallback for development and clean-machine testing.

## Delivery sequence

1. Stabilize realtime subscriptions and assign the next incompatible ABI
   version (expected ABI v6 if the public C surface changes).
2. Build and validate an external runtime ZIP on a clean Windows machine.
3. Add payload manifest/hash verification and extraction tests.
4. Embed the same verified payload into the release DLL.
5. Test first-run extraction, cached startup, concurrent processes, corrupted
   payloads, read-only cache locations, missing VC runtime, shutdown, and
   upgrade/rollback behavior.


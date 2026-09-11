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
point of view, using a bootstrap/core split. The public bootstrap has no
import-time dependency on CPython; it extracts the embedded runtime payload to
a private, content-addressed directory and then loads the core implementation:

```text
mt5_bridge.dll                  // bootstrap, stable C ABI, no Python import
    embedded compressed payload
        -> %LOCALAPPDATA%\\LimiNode\\mt5bridge\\runtime\\<payload-hash>\\
             python311.dll
             python311.zip / Lib/
             site-packages/NumPy/
             site-packages/MetaTrader5/
             dependent runtime DLLs
         mt5_bridge_runtime.dll // current CPython-backed implementation
```

`mt5_bridge_runtime.dll` may link directly to `Python3::Python`, because it is
loaded only after the extracted `python311.dll` is available. The bootstrap
resolves and forwards the exported C ABI through function pointers. Delay-load
Python is not the primary design: it would require proving that no Python
symbol is touched before extraction and loader configuration.

Extraction is performed by `mt5bridge_initialize()`, never from `DllMain`.
The implementation must:

1. derive the payload directory from a fixed payload hash, not from a mutable
   global or the current working directory;
2. verify the embedded payload hash (and, for published builds, its release
   signature) before loading it;
3. validate an existing cache against a signed manifest before reusing it;
4. reject archive entries containing absolute paths, `..`, alternate data
   streams, symlinks, junctions, or other reparse points;
5. use a named process lock and a same-volume staging directory followed by an
   atomic rename; if another process published the final directory first,
   verify and reuse it without overwriting it;
6. configure `PyConfig.home` and module search paths to the extracted runtime;
7. load the core and dependencies with absolute paths and
   `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`.
   Do not call `SetDefaultDllDirectories()` because it changes the host
   process-wide policy. Use `AddDllDirectory()` only when required, retain its
   cookie, and remove it during teardown;
8. retain extracted versions for concurrent/rollback use and clean them only
   through a separate, safe cache-retention policy;
9. report extraction, hash, Python, NumPy, and MetaTrader5 failures through the
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

1. Stabilize realtime subscriptions and assign ABI v6 (completed in the
   realtime branch).
2. Build and validate an external runtime ZIP on a clean Windows machine.
3. Split the current implementation into bootstrap and
   `mt5_bridge_runtime.dll`; verify that the bootstrap has no Python imports.
4. Add payload manifest/hash verification and extraction tests.
5. Embed the same verified payload into the release DLL.
6. Test first-run extraction, cached startup, concurrent processes, corrupted
   payloads, read-only cache locations, missing VC runtime, shutdown, and
   upgrade/rollback behavior.

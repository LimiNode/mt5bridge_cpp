# mt5bridge_cpp

C++17 bridge embedding CPython to call the MetaTrader5 Python API from native apps (quotes, bars, orders). The host-facing surface is the Windows x64 `mt5_bridge.dll` with a small UTF-8 JSON C ABI.

## Quickstart

1. Clone the repository.
2. Build the bridge using MSVC or MinGW.
   - For a one-step MSVC build run `scripts\build_msvc.bat`.
   - Alternatively follow the manual CMake commands below.
3. Prepare the runtime environment (see [Runtime setup](#runtime-setup)).
4. Run `build\bin\Release\usage_example.exe` (Visual Studio) or the
   generator-specific `build\bin\usage_example.exe` to confirm the setup.

## Build

### MSVC

Run the helper script to configure and build a Release binary:

```bat
scripts\build_msvc.bat
```

To invoke CMake manually instead:

```powershell
# From a Developer PowerShell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Project Python environment

The live runtime and Windows smoke tests use the project-local Python 3.11
environment. Create it and install the pinned MetaTrader5/NumPy dependencies
with:

```powershell
.\setup_env.bat
```

Use `venv\Scripts\python.exe` for live checks. The embedded DLL must be built
against the same Python major/minor version as the installed `MetaTrader5`
wheel; configure CMake with `-DPython3_EXECUTABLE=...\venv\Scripts\python.exe`
when more than one Python installation is present.

### MinGW

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

## Runtime setup

1. Install MetaTrader 5 and log into an account.
2. Prepare an embeddable Python runtime. For example:

   ```bash
   python scripts/prepare_py_runtime.py --python-url https://www.python.org/ftp/python/3.11.5/python-3.11.5-embed-amd64.zip --wheels <numpy-wheel-url> <metatrader5-wheel-url> --output build/runtime
   ```

   The preparation script configures the embeddable interpreter's isolated
   `._pth` file and places third-party wheels under `build/runtime/Lib/site-packages`.
   Set `PYTHONHOME`/`PYTHONPATH` to that runtime when launching examples. The
   MetaTrader5 Python package discovers the logged-in terminal through its own
   `initialize()` call; this project does not read a `bridge.ini` file.
4. Run `build\bin\Release\usage_example.exe` (Visual Studio) or
   `build\bin\usage_example.exe` (single-config generators).

For a future distributable release, the supported one-file user experience is
planned as a self-extracting `mt5_bridge.dll` bootstrap. The bootstrap itself
will not link to CPython: it will verify and extract a bundled
`mt5_bridge_runtime.dll` plus the embeddable Python/NumPy/MetaTrader5 runtime
into a content-addressed per-user cache, then load the core during
initialization. The design and clean-machine acceptance checks are recorded in
[ADR-0002](docs/adr/0002-self-contained-runtime-dll.md); the current
development workflow still uses the explicit runtime directory.

## Example usage

```cpp
#include <mt5bridge/client.hpp>
#include <iostream>

int main() {
    mt5bridge::Client bridge;
    try {
        bridge.load();
        bridge.initialize();
        std::cout << bridge.eval(R"({"method":"terminal_info"})") << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The `mt5bridge::Client` header dynamically loads the DLL and does not require
Python, Jansson, or an import library. The plain C declarations are available
in `mt5bridge/abi.h`; high-volume tick/rate POD contracts are in
`mt5bridge/data.h`.

See the `examples` directory for runnable DLL-loading examples and
[`docs/architecture.md`](docs/architecture.md) for the layering and migration
plan. Development rules adapted from the PVS-Studio review of bloated
AI-generated C++ live in [`docs/development-rules.md`](docs/development-rules.md).
High-throughput ticks/rates use the typed POD data plane described in
[`docs/market-data-api.md`](docs/market-data-api.md); known MT5 recovery cases
are tracked in [`docs/mt5-quirks.md`](docs/mt5-quirks.md).

With a logged-in terminal, run the bounded native market-data smoke check from
the build output directory (optionally pass a broker-specific symbol):

```powershell
$env:PYTHONPATH = (Resolve-Path ..\..\..\venv\Lib\site-packages).Path
.\live_market_smoke.exe EURUSD
```

The check reads only the last ten minutes of ticks and one hour of M1 bars,
prints recovery diagnostics, and shuts the bridge down before exiting. Set
`PYTHONPATH` to the project environment that contains `MetaTrader5` when the
embedded runtime cannot discover it automatically.

## Notes

- The current public contract is ABI 7. Tick POD records preserve separate
  integer `volume` and floating-point `volume_real` fields.
- Realtime consumers use `mt5bridge_subscribe_ticks()` and host-driven
  `mt5bridge_process_events()`; overflow is reported as an explicit GAP event.
- Only 64‑bit Windows builds are supported.
- Python 3.11+ is required.
- The C++ client resolves the DLL to an absolute path and restricts dependency
  lookup to the DLL directory and default safe Windows directories.
- The DLL must be shut down before `FreeLibrary`.
- Reinitialization after an owned CPython shutdown requires the documented
  live 100-cycle acceptance test; it is not yet a release guarantee.
- Issues and pull requests are welcome.

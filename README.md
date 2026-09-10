# mt5bridge_cpp

C++17 bridge embedding CPython to call the MetaTrader5 Python API from native apps (quotes, bars, orders). The host-facing surface is the Windows x64 `mt5_bridge.dll` with a small UTF-8 JSON C ABI.

## Quickstart

1. Clone the repository.
2. Build the bridge using MSVC or MinGW.
   - For a one-step MSVC build run `scripts\build_msvc.bat`.
   - Alternatively follow the manual CMake commands below.
3. Prepare the runtime environment (see [Runtime setup](#runtime-setup)).
4. Run `build\bin\usage_example.exe` to confirm the setup.

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

### MinGW

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

## Runtime setup

1. Install MetaTrader 5 and log into an account.
2. Prepare an embeddable Python runtime. For example:

   ```bash
   python scripts/prepare_py_runtime.py --python-url https://www.python.org/ftp/python/3.11.5/python-3.11.5-embed-amd64.zip --output python
   ```

   Ensure the resulting `python` folder accompanies your application or set `PYTHONHOME` to that path.
3. Create a `bridge.ini` with the MT5 terminal path:

   ```ini
   terminal_path=C:\Path\To\MetaTrader5
   ```
4. Run `build\bin\usage_example.exe` from the build directory to verify the setup.

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

## Notes

- The current public contract is ABI 4. Tick POD records preserve separate
  integer `volume` and floating-point `volume_real` fields.
- Only 64‑bit Windows builds are supported.
- Python 3.11+ is required.
- The C++ client resolves the DLL to an absolute path and restricts dependency
  lookup to the DLL directory and default safe Windows directories.
- The DLL must be shut down before `FreeLibrary`.
- Reinitialization after an owned CPython shutdown requires the documented
  live 100-cycle acceptance test; it is not yet a release guarantee.
- Issues and pull requests are welcome.

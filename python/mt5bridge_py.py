# MIT License
#
# Copyright (c) 2025 Aster Seker
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

## \file mt5bridge_py.py
#  \brief Provides a thin ctypes binding to the mt5bridge control-plane ABI.

"""Python bindings to the mt5bridge C++ library."""

from __future__ import annotations

import ctypes
import json
from ctypes import POINTER, byref, c_char_p, c_int, c_void_p, c_wchar_p

## \brief ABI version required by this ctypes adapter.
_ABI_VERSION = 4

# Load the mt5bridge shared library.
_lib = ctypes.WinDLL("mt5_bridge.dll")

# Configure argument and result types for exported functions.
_lib.mt5bridge_abi_version.argtypes = []
_lib.mt5bridge_abi_version.restype = ctypes.c_uint32
_lib.mt5bridge_initialize.argtypes = [c_wchar_p]
_lib.mt5bridge_initialize.restype = c_int
_lib.mt5bridge_shutdown.argtypes = []
_lib.mt5bridge_shutdown.restype = None
_lib.mt5bridge_eval_json.argtypes = [c_char_p, POINTER(c_void_p)]
_lib.mt5bridge_eval_json.restype = c_int
_lib.mt5bridge_free.argtypes = [c_void_p]
_lib.mt5bridge_free.restype = None
_lib.mt5bridge_last_error.argtypes = []
_lib.mt5bridge_last_error.restype = c_char_p

if _lib.mt5bridge_abi_version() != _ABI_VERSION:
    raise RuntimeError("incompatible mt5_bridge.dll ABI version")


## \brief Raises the current DLL error when an ABI call fails.
#  \param code Status code returned by an mt5bridge function.
def _check_error(code: int) -> None:
    """Raise RuntimeError if ``code`` indicates failure."""
    if code != 0:
        err = _lib.mt5bridge_last_error()
        msg = err.decode("utf-8") if err else "unknown error"
        raise RuntimeError(msg)


## \brief Initializes the bridge with an explicit Python home directory.
#  \param python_home Python runtime directory used by embedded CPython.
def init(python_home: str) -> None:
    """Initialize the bridge runtime using the provided Python home path."""
    _check_error(_lib.mt5bridge_initialize(python_home))


## \brief Shuts down the MetaTrader connection and embedded runtime.
def shutdown() -> None:
    """Shut down the bridge runtime, freeing resources."""
    _lib.mt5bridge_shutdown()


## \brief Executes a control-plane request and copies the DLL-owned response.
#  \param request JSON-compatible request dictionary.
#  \return UTF-8 JSON response as a Python string.
def _eval(request: dict) -> str:
    """Send *request* to the bridge and return the JSON response string."""
    request_json = json.dumps(request).encode("utf-8")
    response = c_void_p()
    _check_error(_lib.mt5bridge_eval_json(request_json, byref(response)))
    if not response.value:
        err = _lib.mt5bridge_last_error()
        msg = err.decode("utf-8") if err else "unknown error"
        raise RuntimeError(msg)
    try:
        return ctypes.string_at(response.value).decode("utf-8")
    finally:
        _lib.mt5bridge_free(response)


## \brief Returns the latest M1 bars as JSON.
#  \param symbol MetaTrader symbol name.
#  \param count Maximum number of requested bars.
#  \return UTF-8 JSON array returned by the bridge.
def get_m1_bars_json(symbol: str, count: int) -> str:
    """Return the latest *count* M1 bars for *symbol* as a JSON string."""
    return _eval({"method": "get_m1_bars", "symbol": symbol, "count": count})


## \brief Submits one market buy request without automatic retry.
#  \param symbol MetaTrader symbol name.
#  \param volume Order volume in lots.
#  \return UTF-8 JSON representation of the MetaTrader result.
def open_market_buy(symbol: str, volume: float) -> str:
    """Open a market buy order for *symbol* with *volume* lots."""
    return _eval({"method": "open_market_buy", "symbol": symbol, "volume": volume})

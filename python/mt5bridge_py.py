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
import os
from ctypes import (
    POINTER,
    Structure,
    byref,
    c_char,
    c_char_p,
    c_double,
    c_int,
    c_int64,
    c_uint32,
    c_uint64,
    c_void_p,
    c_wchar_p,
)

## \brief ABI version required by this ctypes adapter.
_ABI_VERSION = 8
_TRADE_API_VERSION = 2


class Mt5OrdersRequest(Structure):
    """Filters an active-order observation query."""

    _fields_ = [("symbol_utf8", c_char_p), ("group_utf8", c_char_p), ("ticket", c_uint64), ("reserved", c_uint32)]


class Mt5PositionsRequest(Structure):
    """Filters an active-position observation query."""

    _fields_ = [
        ("symbol_utf8", c_char_p),
        ("group_utf8", c_char_p),
        ("ticket", c_uint64),
        ("identifier", c_uint64),
        ("reserved", c_uint32),
    ]


class Mt5HistoryRequest(Structure):
    """Bounds a history order/deal observation query in UTC milliseconds."""

    _fields_ = [
        ("from_msc", c_int64),
        ("to_msc", c_int64),
        ("group_utf8", c_char_p),
        ("ticket", c_uint64),
        ("position_id", c_uint64),
        ("reserved", c_uint32),
    ]


class Mt5OrderSnapshot(Structure):
    """Typed order/history-order evidence returned by the DLL."""

    _fields_ = [
        ("ticket", c_uint64), ("position_id", c_uint64), ("position_by_id", c_uint64),
        ("magic", c_uint64), ("type", c_uint32), ("state", c_uint32),
        ("reason", c_uint32), ("type_time", c_uint32), ("type_filling", c_uint32),
        ("volume_initial", c_double), ("volume_current", c_double),
        ("price_open", c_double), ("price_current", c_double), ("price_stoplimit", c_double),
        ("sl", c_double), ("tp", c_double), ("time_setup_msc", c_int64),
        ("time_done_msc", c_int64), ("time_expiration_msc", c_int64),
        ("symbol", c_char * 64), ("comment", c_char * 128), ("external_id", c_char * 128),
        ("known_fields", c_uint64), ("reserved", c_uint32 * 2),
    ]


# History orders intentionally use the same evidence shape as active orders.
Mt5HistoryOrderSnapshot = Mt5OrderSnapshot


class Mt5PositionSnapshot(Structure):
    """Typed active-position evidence returned by the DLL."""

    _fields_ = [
        ("ticket", c_uint64), ("identifier", c_uint64), ("magic", c_uint64),
        ("type", c_uint32), ("reason", c_uint32), ("volume", c_double),
        ("price_open", c_double), ("price_current", c_double), ("sl", c_double),
        ("tp", c_double), ("profit", c_double), ("swap", c_double),
        ("time_msc", c_int64), ("time_update_msc", c_int64), ("symbol", c_char * 64),
        ("comment", c_char * 128), ("external_id", c_char * 128),
        ("known_fields", c_uint64), ("reserved", c_uint32 * 2),
    ]


class Mt5DealSnapshot(Structure):
    """Typed history-deal evidence returned by the DLL."""

    _fields_ = [
        ("ticket", c_uint64), ("order_ticket", c_uint64), ("position_id", c_uint64),
        ("magic", c_uint64), ("type", c_uint32), ("entry", c_uint32), ("reason", c_uint32),
        ("volume", c_double), ("price", c_double), ("profit", c_double),
        ("commission", c_double), ("swap", c_double), ("fee", c_double),
        ("time_msc", c_int64), ("symbol", c_char * 64), ("comment", c_char * 128),
        ("external_id", c_char * 128), ("known_fields", c_uint64), ("reserved", c_uint32 * 2),
    ]

# Load the mt5bridge shared library.
_lib = ctypes.WinDLL(os.environ.get("MT5BRIDGE_DLL", "mt5_bridge.dll"))

# Configure argument and result types for exported functions.
_lib.mt5bridge_abi_version.argtypes = []
_lib.mt5bridge_abi_version.restype = ctypes.c_uint32
_lib.mt5bridge_initialize.argtypes = [c_wchar_p]
_lib.mt5bridge_initialize.restype = c_int
_lib.mt5bridge_shutdown.argtypes = []
_lib.mt5bridge_shutdown.restype = c_int
_lib.mt5bridge_eval_json.argtypes = [c_char_p, POINTER(c_void_p)]
_lib.mt5bridge_eval_json.restype = c_int
_lib.mt5bridge_free.argtypes = [c_void_p]
_lib.mt5bridge_free.restype = None
_lib.mt5bridge_last_error.argtypes = []
_lib.mt5bridge_last_error.restype = c_char_p
_lib.mt5bridge_trade_api_version.argtypes = []
_lib.mt5bridge_trade_api_version.restype = c_uint32

if _lib.mt5bridge_abi_version() != _ABI_VERSION:
    raise RuntimeError("incompatible mt5_bridge.dll ABI version")
if _lib.mt5bridge_trade_api_version() != _TRADE_API_VERSION:
    raise RuntimeError("incompatible mt5_bridge trade API version")


_lib.mt5bridge_query_orders.argtypes = [POINTER(Mt5OrdersRequest), POINTER(c_void_p)]
_lib.mt5bridge_query_orders.restype = c_int
_lib.mt5bridge_order_buffer_data.argtypes = [c_void_p]
_lib.mt5bridge_order_buffer_data.restype = POINTER(Mt5OrderSnapshot)
_lib.mt5bridge_order_buffer_size.argtypes = [c_void_p]
_lib.mt5bridge_order_buffer_size.restype = ctypes.c_size_t
_lib.mt5bridge_order_buffer_free.argtypes = [c_void_p]
_lib.mt5bridge_order_buffer_free.restype = None
_lib.mt5bridge_query_positions.argtypes = [POINTER(Mt5PositionsRequest), POINTER(c_void_p)]
_lib.mt5bridge_query_positions.restype = c_int
_lib.mt5bridge_position_buffer_data.argtypes = [c_void_p]
_lib.mt5bridge_position_buffer_data.restype = POINTER(Mt5PositionSnapshot)
_lib.mt5bridge_position_buffer_size.argtypes = [c_void_p]
_lib.mt5bridge_position_buffer_size.restype = ctypes.c_size_t
_lib.mt5bridge_position_buffer_free.argtypes = [c_void_p]
_lib.mt5bridge_position_buffer_free.restype = None
_lib.mt5bridge_query_history_orders.argtypes = [POINTER(Mt5HistoryRequest), POINTER(c_void_p)]
_lib.mt5bridge_query_history_orders.restype = c_int
_lib.mt5bridge_history_order_buffer_data.argtypes = [c_void_p]
_lib.mt5bridge_history_order_buffer_data.restype = POINTER(Mt5OrderSnapshot)
_lib.mt5bridge_history_order_buffer_size.argtypes = [c_void_p]
_lib.mt5bridge_history_order_buffer_size.restype = ctypes.c_size_t
_lib.mt5bridge_history_order_buffer_free.argtypes = [c_void_p]
_lib.mt5bridge_history_order_buffer_free.restype = None
_lib.mt5bridge_query_history_deals.argtypes = [POINTER(Mt5HistoryRequest), POINTER(c_void_p)]
_lib.mt5bridge_query_history_deals.restype = c_int
_lib.mt5bridge_deal_buffer_data.argtypes = [c_void_p]
_lib.mt5bridge_deal_buffer_data.restype = POINTER(Mt5DealSnapshot)
_lib.mt5bridge_deal_buffer_size.argtypes = [c_void_p]
_lib.mt5bridge_deal_buffer_size.restype = ctypes.c_size_t
_lib.mt5bridge_deal_buffer_free.argtypes = [c_void_p]
_lib.mt5bridge_deal_buffer_free.restype = None


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
    _check_error(_lib.mt5bridge_shutdown())


def _utf8(value: str | None) -> tuple[bytes | None, c_char_p]:
    """Keep encoded storage alive while a request crosses the DLL boundary."""
    encoded = value.encode("utf-8") if value else None
    return encoded, c_char_p(encoded) if encoded else c_char_p()


def _read_buffer(query, data, size, release, request, snapshot_type):
    """Run one typed query and copy its DLL-owned buffer before releasing it."""
    buffer = c_void_p()
    _check_error(query(byref(request), byref(buffer)))
    try:
        count = int(size(buffer))
        pointer = data(buffer)
        values = []
        for index in range(count):
            value = snapshot_type()
            ctypes.memmove(ctypes.byref(value), ctypes.addressof(pointer[index]), ctypes.sizeof(value))
            values.append(value)
        return values
    finally:
        release(buffer)


def orders(symbol: str | None = None, group: str | None = None, ticket: int = 0):
    """Return typed active-order snapshots."""
    symbol_bytes, symbol_ptr = _utf8(symbol)
    group_bytes, group_ptr = _utf8(group)
    request = Mt5OrdersRequest(symbol_ptr, group_ptr, ticket, 0)
    # Retain local byte objects until the call returns.
    _ = (symbol_bytes, group_bytes)
    return _read_buffer(
        _lib.mt5bridge_query_orders,
        _lib.mt5bridge_order_buffer_data,
        _lib.mt5bridge_order_buffer_size,
        _lib.mt5bridge_order_buffer_free,
        request,
        Mt5OrderSnapshot,
    )


def positions(
    symbol: str | None = None,
    group: str | None = None,
    ticket: int = 0,
    identifier: int = 0,
):
    """Return typed active-position snapshots."""
    symbol_bytes, symbol_ptr = _utf8(symbol)
    group_bytes, group_ptr = _utf8(group)
    request = Mt5PositionsRequest(symbol_ptr, group_ptr, ticket, identifier, 0)
    _ = (symbol_bytes, group_bytes)
    return _read_buffer(
        _lib.mt5bridge_query_positions,
        _lib.mt5bridge_position_buffer_data,
        _lib.mt5bridge_position_buffer_size,
        _lib.mt5bridge_position_buffer_free,
        request,
        Mt5PositionSnapshot,
    )


def history_orders(from_msc: int, to_msc: int, group: str | None = None,
                   ticket: int = 0, position_id: int = 0):
    """Return typed history-order snapshots for an inclusive UTC range."""
    group_bytes, group_ptr = _utf8(group)
    request = Mt5HistoryRequest(from_msc, to_msc, group_ptr, ticket, position_id, 0)
    _ = group_bytes
    return _read_buffer(
        _lib.mt5bridge_query_history_orders,
        _lib.mt5bridge_history_order_buffer_data,
        _lib.mt5bridge_history_order_buffer_size,
        _lib.mt5bridge_history_order_buffer_free,
        request,
        Mt5OrderSnapshot,
    )


def history_deals(from_msc: int, to_msc: int, group: str | None = None,
                  ticket: int = 0, position_id: int = 0):
    """Return typed history-deal snapshots for an inclusive UTC range."""
    group_bytes, group_ptr = _utf8(group)
    request = Mt5HistoryRequest(from_msc, to_msc, group_ptr, ticket, position_id, 0)
    _ = group_bytes
    return _read_buffer(
        _lib.mt5bridge_query_history_deals,
        _lib.mt5bridge_deal_buffer_data,
        _lib.mt5bridge_deal_buffer_size,
        _lib.mt5bridge_deal_buffer_free,
        request,
        Mt5DealSnapshot,
    )


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

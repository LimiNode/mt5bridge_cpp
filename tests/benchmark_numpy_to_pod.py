## \file benchmark_numpy_to_pod.py
#  \brief Measures NumPy-to-POD conversion through the public DLL ABI.

"""Benchmarks the fake-MT5 NumPy-to-POD data path through the public DLL ABI.

Build the runtime with the same Python version as this process, then run:

    $env:MT5BRIDGE_DLL = "...\\mt5_bridge.dll"
    python tests/benchmark_numpy_to_pod.py

The slice baseline measures only deterministic in-process NumPy page lookup.
The DLL timings include Python calls, POD conversion, pagination, and either
result-buffer accumulation or callback delivery; they do not model terminal
IPC or broker history latency.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import statistics
import sys
import time
import types
from ctypes import POINTER, Structure, byref, c_char_p, c_double, c_int, c_int64
from ctypes import c_size_t, c_uint32, c_uint64, c_void_p

import numpy as np


class Mt5TicksRequest(Structure):
    """Matches the ABI 7 tick request."""

    _fields_ = [
        ("symbol_utf8", c_char_p),
        ("from_msc", c_int64),
        ("to_msc", c_int64),
        ("flags", c_uint32),
        ("reserved", c_uint32),
    ]


class Mt5Tick(Structure):
    """Matches the ABI 7 tick record."""

    _fields_ = [
        ("time_msc", c_int64),
        ("bid", c_double),
        ("ask", c_double),
        ("last", c_double),
        ("volume", c_uint64),
        ("volume_real", c_double),
        ("flags", c_uint32),
        ("reserved", c_uint32),
    ]


## \brief Structured NumPy layout returned by the benchmark's fake MT5 module.
TICK_DTYPE = np.dtype(
    [
        ("time", "<i8"),
        ("time_msc", "<i8"),
        ("bid", "<f8"),
        ("ask", "<f8"),
        ("last", "<f8"),
        ("volume", "<u8"),
        ("volume_real", "<f8"),
        ("flags", "<u4"),
    ]
)


def make_ticks(count: int) -> np.ndarray:
    """Creates sorted structured ticks without Python row objects."""
    values = np.zeros(count, dtype=TICK_DTYPE)
    sequence = np.arange(1, count + 1, dtype=np.int64)
    values["time"] = sequence // 1000
    values["time_msc"] = sequence
    values["bid"] = 1.1
    values["ask"] = 1.2
    values["last"] = 1.15
    values["volume"] = sequence
    values["volume_real"] = sequence / 10.0
    values["flags"] = 1
    return values


def make_fake_module(values: np.ndarray) -> types.ModuleType:
    """Creates a paging MetaTrader5 module backed by one NumPy array."""
    module = types.ModuleType("MetaTrader5")
    module.COPY_TICKS_ALL = 3
    module.pages = 0
    timestamps = np.ascontiguousarray(values["time_msc"])

    def initialize() -> bool:
        return True

    def shutdown() -> bool:
        return True

    def last_error() -> tuple[int, str]:
        return (1, "Success")

    def copy_ticks_from(symbol: str, when: object, count: int, flags: int) -> np.ndarray:
        del symbol, flags
        module.pages += 1
        cursor = int(when.timestamp() * 1000)
        offset = int(np.searchsorted(timestamps, cursor, side="left"))
        return values[offset : offset + count]

    module.initialize = initialize
    module.shutdown = shutdown
    module.last_error = last_error
    module.copy_ticks_from = copy_ticks_from
    return module


def load_runtime(path: str) -> tuple[ctypes.WinDLL, type[ctypes._CFuncPtr]]:
    """Loads ABI 7 and declares the benchmarked exports."""
    module = ctypes.WinDLL(path)
    module.mt5bridge_abi_version.restype = c_uint32
    if module.mt5bridge_abi_version() != 7:
        raise RuntimeError("benchmark requires ABI version 7")
    module.mt5bridge_initialize.argtypes = [ctypes.c_wchar_p]
    module.mt5bridge_initialize.restype = c_int
    module.mt5bridge_shutdown.argtypes = []
    module.mt5bridge_shutdown.restype = c_int
    module.mt5bridge_query_ticks.argtypes = [POINTER(Mt5TicksRequest), POINTER(c_void_p)]
    module.mt5bridge_query_ticks.restype = c_int
    module.mt5bridge_tick_buffer_size.argtypes = [c_void_p]
    module.mt5bridge_tick_buffer_size.restype = c_size_t
    module.mt5bridge_tick_buffer_free.argtypes = [c_void_p]
    callback_type = ctypes.CFUNCTYPE(c_int, POINTER(Mt5Tick), c_size_t, c_void_p)
    module.mt5bridge_copy_ticks_range.argtypes = [
        POINTER(Mt5TicksRequest),
        c_size_t,
        callback_type,
        c_void_p,
    ]
    module.mt5bridge_copy_ticks_range.restype = c_int
    return module, callback_type


def measure_slice_baseline(values: np.ndarray) -> float:
    """Measures inclusive page lookup and slicing without the DLL."""
    cursor = 1
    skipped = 0
    timestamps = np.ascontiguousarray(values["time_msc"])
    started = time.perf_counter()
    while True:
        count = 65536 + skipped
        offset = int(np.searchsorted(timestamps, cursor, side="left"))
        page = values[offset : offset + count]
        if not page.size:
            break
        last = int(page[-1]["time_msc"])
        trailing = int(page.size - np.searchsorted(page["time_msc"], last, side="left"))
        if page.size < count:
            break
        cursor = last
        skipped = trailing
    return time.perf_counter() - started


def measure_buffer(module: ctypes.WinDLL, values: np.ndarray) -> tuple[float, int]:
    """Measures conversion plus accumulation in a DLL-owned POD buffer."""
    fake = make_fake_module(values)
    sys.modules["MetaTrader5"] = fake
    if module.mt5bridge_initialize(None) != 0:
        raise RuntimeError("bridge initialization failed")
    request = Mt5TicksRequest(b"BENCH", 1, values.size, 0, 0)
    result = c_void_p()
    started = time.perf_counter()
    status = module.mt5bridge_query_ticks(byref(request), byref(result))
    elapsed = time.perf_counter() - started
    size = module.mt5bridge_tick_buffer_size(result) if result.value else 0
    if result.value:
        module.mt5bridge_tick_buffer_free(result)
    module.mt5bridge_shutdown()
    sys.modules.pop("MetaTrader5", None)
    if status != 0 or size != values.size:
        raise RuntimeError(f"buffer benchmark failed: status={status}, size={size}")
    return elapsed, fake.pages


def measure_callback(
    module: ctypes.WinDLL,
    callback_type: type[ctypes._CFuncPtr],
    values: np.ndarray,
) -> tuple[float, int]:
    """Measures conversion plus bounded callback delivery."""
    fake = make_fake_module(values)
    sys.modules["MetaTrader5"] = fake
    if module.mt5bridge_initialize(None) != 0:
        raise RuntimeError("bridge initialization failed")
    delivered = 0

    def consume(ticks: POINTER(Mt5Tick), count: int, user_data: int) -> int:
        del ticks, user_data
        nonlocal delivered
        delivered += count
        return 0

    callback = callback_type(consume)
    request = Mt5TicksRequest(b"BENCH", 1, values.size, 0, 0)
    started = time.perf_counter()
    status = module.mt5bridge_copy_ticks_range(byref(request), 65536, callback, None)
    elapsed = time.perf_counter() - started
    module.mt5bridge_shutdown()
    sys.modules.pop("MetaTrader5", None)
    if status != 0 or delivered != values.size:
        raise RuntimeError(f"callback benchmark failed: status={status}, size={delivered}")
    return elapsed, fake.pages


def main() -> int:
    """Runs the selected sizes and prints median wall-clock measurements."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--sizes", nargs="+", type=int, default=[100_000, 1_000_000, 5_000_000])
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    dll_path = os.environ.get("MT5BRIDGE_DLL")
    if not dll_path:
        parser.error("set MT5BRIDGE_DLL to a built mt5_bridge.dll")
    module, callback_type = load_runtime(dll_path)
    print("ticks,numpy_mib,pages,slice_ms,buffer_ms,callback_ms,buffer_mticks_s,callback_mticks_s")
    for count in args.sizes:
        values = make_ticks(count)
        slices: list[float] = []
        buffers: list[float] = []
        callbacks: list[float] = []
        pages = 0
        for _ in range(args.repeat):
            slices.append(measure_slice_baseline(values))
            buffer_time, pages = measure_buffer(module, values)
            callback_time, _ = measure_callback(module, callback_type, values)
            buffers.append(buffer_time)
            callbacks.append(callback_time)
        slice_time = statistics.median(slices)
        buffer_time = statistics.median(buffers)
        callback_time = statistics.median(callbacks)
        print(
            f"{count},{values.nbytes / 1048576:.1f},{pages},"
            f"{slice_time * 1000:.3f},{buffer_time * 1000:.3f},"
            f"{callback_time * 1000:.3f},{count / buffer_time / 1e6:.3f},"
            f"{count / callback_time / 1e6:.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

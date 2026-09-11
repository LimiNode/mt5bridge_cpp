"""Minimal MetaTrader5 module used by the owned-interpreter smoke test."""

COPY_TICKS_ALL = 3

import numpy as np
from datetime import datetime, timezone

_TICK_DTYPE = np.dtype([
    ("time", "<i8"), ("time_msc", "<i8"), ("bid", "<f8"),
    ("ask", "<f8"), ("last", "<f8"), ("volume", "<u8"),
    ("volume_real", "<f8"), ("flags", "<u4")
])


def initialize() -> bool:
    """Accepts bridge initialization."""
    return True


def shutdown() -> bool:
    """Accepts bridge shutdown."""
    return True


def last_error() -> tuple[int, str]:
    """Reports a successful fake terminal status."""
    return (1, "Success")


def terminal_info() -> dict[str, object]:
    """Returns a deterministic response without importing NumPy."""
    return {"connected": True, "owned_interpreter": True}


def copy_ticks_from(symbol: str, when: datetime, count: int, flags: int) -> np.ndarray:
    """Returns one current tick so the native poller must become READY."""
    del symbol, flags
    now_msc = int(datetime.now(timezone.utc).timestamp() * 1000)
    start_msc = int(when.timestamp() * 1000)
    if start_msc > now_msc:
        return np.empty(0, dtype=_TICK_DTYPE)
    values = np.zeros(max(1, min(count, 1)), dtype=_TICK_DTYPE)
    values["time"] = now_msc // 1000
    values["time_msc"] = now_msc
    values["bid"] = 1.1
    values["ask"] = 1.2
    values["last"] = 1.15
    values["volume"] = 1
    values["volume_real"] = 1.0
    values["flags"] = COPY_TICKS_ALL
    return values

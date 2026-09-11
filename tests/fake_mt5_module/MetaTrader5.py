"""Minimal MetaTrader5 module used by the owned-interpreter smoke test."""

COPY_TICKS_ALL = 3


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

"""Deterministic MetaTrader5 module for the owner/transport integration test."""

import os

_call_count_file = os.environ.get("MT5BRIDGE_CALL_COUNT_FILE")
_send_mode = os.environ.get("MT5BRIDGE_ORDER_SEND_MODE", "success")


def _read_call_count() -> int:
    if not _call_count_file:
        return 0
    try:
        with open(_call_count_file, "r", encoding="ascii") as stream:
            return int(stream.read() or "0")
    except (OSError, ValueError):
        return 0


order_calls = _read_call_count()


def account_info() -> dict[str, object]:
    """Return the account identity used by the durable owner fixture."""
    return {"server": "Fake-Server", "login": 42}


def order_send(request: dict[str, object]) -> dict[str, object] | None:
    """Accept one request and return a complete MqlTradeResult-shaped mapping."""
    global order_calls
    order_calls += 1
    if _call_count_file:
        with open(_call_count_file, "w", encoding="ascii") as stream:
            stream.write(str(order_calls))
    if _send_mode == "none":
        return None
    return {
        "retcode": 10009,
        "deal": 7002,
        "order": 7001,
        "volume": float(request["volume"]),
        "price": 1.1,
        "bid": 1.099,
        "ask": 1.101,
        "comment": "integration success",
        "request_id": 7,
        "retcode_external": 0,
        "request": request,
    }

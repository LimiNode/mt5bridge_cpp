"""Deterministic MetaTrader5 module for the owner/transport integration test."""

import os

def _read_call_count() -> int:
    call_count_file = os.environ.get("MT5BRIDGE_CALL_COUNT_FILE")
    if not call_count_file:
        return 0
    if not os.path.exists(call_count_file):
        if os.environ.get("MT5BRIDGE_EXPECT_EXISTING_COUNT") == "1":
            raise RuntimeError("durable order call counter is missing")
        return 0
    try:
        with open(call_count_file, "r", encoding="ascii") as stream:
            value = int(stream.read())
    except (OSError, ValueError) as error:
        raise RuntimeError("durable order call counter is unreadable") from error
    if value < 0:
        raise RuntimeError("durable order call counter is negative")
    return value


order_calls = _read_call_count()


def account_info() -> dict[str, object]:
    """Return the account identity used by the durable owner fixture."""
    return {
        "server": os.environ.get("MT5BRIDGE_ACCOUNT_SERVER", "Fake-Server"),
        "login": int(os.environ.get("MT5BRIDGE_ACCOUNT_LOGIN", "42")),
    }


def order_send(request: dict[str, object]) -> dict[str, object] | None:
    """Accept one request and return a complete MqlTradeResult-shaped mapping."""
    global order_calls
    order_calls = _read_call_count() + 1
    call_count_file = os.environ.get("MT5BRIDGE_CALL_COUNT_FILE")
    if call_count_file:
        with open(call_count_file, "w", encoding="ascii") as stream:
            stream.write(str(order_calls))
    send_mode = os.environ.get("MT5BRIDGE_ORDER_SEND_MODE", "success")
    if send_mode == "none":
        return None
    if send_mode == "exception":
        raise RuntimeError("simulated Python transport failure")
    if send_mode == "malformed":
        return {"retcode": 10009}
    retcode = 10018 if send_mode == "market_closed" else 10009
    return {
        "retcode": retcode,
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

"""Deterministic MetaTrader5 module for the owner/transport integration test."""

order_calls = 0


def account_info() -> dict[str, object]:
    """Return the account identity used by the durable owner fixture."""
    return {"server": "Fake-Server", "login": 42}


def order_send(request: dict[str, object]) -> dict[str, object]:
    """Accept one request and return a complete MqlTradeResult-shaped mapping."""
    global order_calls
    order_calls += 1
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

"""Deterministic, dependency-free broker facts for the private collector tests."""

import math
import time
from collections import namedtuple


_scenario = "empty"
_calls = {}
_send_calls = 0
_account = {}
_terminal = {}
_symbol = {}
_positions = []
_orders = []


def _count(name):
    """Count one observed source operation without altering broker facts."""
    _calls[name] = _calls.get(name, 0) + 1
    return _calls[name]


def _record(fields):
    """Exercise MT5-style attribute records as well as strict dictionary fields."""
    if _scenario == "namedtuples":
        return namedtuple("BrokerRecord", fields)(**fields)
    return fields


def _position(ticket, side=0, volume=0.03, symbol="EURUSD"):
    """Create a complete position with a distinct stable identifier."""
    return dict(ticket=ticket, identifier=ticket + 10000, type=side,
                symbol=symbol, volume=volume)


def _order(ticket, side=0, volume=0.01, symbol="EURUSD", state=1):
    """Create an active order whose remaining quantity is explicit."""
    return dict(ticket=ticket, type=side, state=state, symbol=symbol,
                volume_current=volume)


def configure(scenario):
    """Reset one source scenario, but never reset the lifetime send audit."""
    global _scenario, _calls, _account, _terminal, _symbol, _positions, _orders
    _scenario = scenario
    _calls = {}
    _account = dict(server="Collector-Demo", login=424242, margin_mode=2,
                    trade_mode=0, trade_allowed=True, trade_expert=True)
    _terminal = dict(connected=True, trade_allowed=True, tradeapi_disabled=False)
    _symbol = dict(name="EURUSD", trade_mode=4, trade_exemode=2,
                   order_mode=0x7f, filling_mode=3, volume_min=0.01,
                   volume_max=100.0, volume_step=0.01, volume_limit=0.0)
    _positions = []
    _orders = []

    if scenario.startswith("missing-"):
        _, source, name = scenario.split("-", 2)
        {"account": _account, "terminal": _terminal, "symbol": _symbol}[source].pop(name)
    elif scenario.startswith("mode-"):
        _symbol["trade_exemode"] = int(scenario[5:])
    elif scenario == "zero-filling":
        _symbol["filling_mode"] = 0
    elif scenario in ("quarter", "integer"):
        step = 0.25 if scenario == "quarter" else 1.0
        _symbol.update(volume_min=step, volume_max=100.0, volume_step=step,
                       volume_limit=200.0)
    elif scenario in ("books", "permuted", "reduced-limit", "namedtuples"):
        _positions = [_position(101), _position(102, 1, 0.04)]
        states = (0, 1, 3, 7, 8, 9, 1, 3)
        _orders = [_order(201 + side, side, (side + 1) / 100.0,
                          state=states[side]) for side in range(8)]
        if scenario == "reduced-limit":
            _symbol["volume_limit"] = 0.01
    elif scenario in ("netting-exposure", "hedging-exposure"):
        _account["margin_mode"] = 0 if scenario == "netting-exposure" else 2
        _positions = [_position(101, 1)]
    elif scenario in ("unrelated", "malformed-unrelated"):
        _positions = [_position(101, symbol="GBPUSD", volume=0.005)]
        _orders = [_order(201, 7, 0.005, symbol="GBPUSD")]
        if scenario == "malformed-unrelated":
            del _orders[0]["volume_current"]
    elif scenario.startswith("account-"):
        name = scenario[8:]
        mutations = {
            "wrong-server": ("server", "Foreign-Demo"),
            "wrong-login": ("login", 424243),
            "nul": ("server", "Collector-Demo\0foreign"),
            "control": ("server", "Collector\nDemo"),
            "truncated": ("server", "s" * 1024),
            "text-login": ("login", "424242"),
            "bool-login": ("login", True),
            "unknown-margin": ("margin_mode", 3),
            "unknown-trade": ("trade_mode", 3),
            "int-permission": ("trade_allowed", 1),
            "denied": ("trade_allowed", False),
            "expert-denied": ("trade_expert", False),
        }
        if name in mutations:
            field, value = mutations[name]
            _account[field] = value
    elif scenario.startswith("terminal-"):
        mutations = {
            "terminal-int-permission": ("trade_allowed", 1),
            "terminal-disconnected": ("connected", False),
            "terminal-denied": ("trade_allowed", False),
            "terminal-api-disabled": ("tradeapi_disabled", True),
        }
        if scenario in mutations:
            field, value = mutations[scenario]
            _terminal[field] = value
    elif scenario.startswith("symbol-"):
        mutations = {
            "symbol-wrong": ("name", "GBPUSD"),
            "symbol-nul": ("name", "EURUSD\0alias"),
            "symbol-control": ("name", "EUR\nUSD"),
            "symbol-truncated": ("name", "s" * 1024),
            "symbol-text-step": ("volume_step", "0.01"),
            "symbol-bool-mode": ("trade_exemode", True),
            "symbol-unknown-execution": ("trade_exemode", 4),
            "symbol-unknown-trade": ("trade_mode", 5),
            "symbol-unknown-order": ("order_mode", 0x80),
            "symbol-unknown-filling": ("filling_mode", 0x08),
            "symbol-nan": ("volume_step", float("nan")),
            "symbol-inf": ("volume_max", float("inf")),
            "symbol-zero-step": ("volume_step", 0.0),
            "symbol-negative-limit": ("volume_limit", -1.0),
            "symbol-negative-zero-limit": ("volume_limit", -0.0),
            "symbol-tiny-step": ("volume_step", 1e-10),
            "symbol-ambiguous-step": ("volume_step", float(2 ** 53)),
            "symbol-nextafter-step": ("volume_step", math.nextafter(0.01, math.inf)),
            "symbol-nextafter-min": ("volume_min", math.nextafter(0.01, math.inf)),
            "symbol-nongrid-max": ("volume_max", 1.005),
            "symbol-nongrid-limit": ("volume_limit", 1.005),
        }
        if scenario in mutations:
            field, value = mutations[scenario]
            _symbol[field] = value
    elif scenario in ("duplicate-position", "duplicate-identifier", "duplicate-order"):
        if scenario == "duplicate-order":
            _orders = [_order(201), _order(201)]
        else:
            _positions = [_position(101), _position(101 if scenario == "duplicate-position" else 102)]
            if scenario == "duplicate-identifier":
                _positions[1]["identifier"] = _positions[0]["identifier"]
    elif scenario in ("replace-position", "replace-order", "change-volume"):
        _positions = [_position(101)]
        _orders = [_order(201)]
    elif scenario.startswith("row-"):
        _positions = [_position(101)]
        mutations = {
            "row-zero-ticket": ("ticket", 0),
            "row-bool-ticket": ("ticket", True),
            "row-unknown-position": ("type", 2),
            "row-zero-identifier": ("identifier", 0),
            "row-zero-volume": ("volume", 0.0),
            "row-nan": ("volume", float("nan")),
            "row-inf": ("volume", float("inf")),
            "row-nongrid": ("volume", 0.015),
            "row-nextafter": ("volume", math.nextafter(0.03, math.inf)),
        }
        field, value = mutations[scenario]
        _positions[0][field] = value
    elif scenario in ("order-close-by", "order-inactive", "order-missing-state"):
        _orders = [_order(201)]
        if scenario == "order-close-by":
            _orders[0]["type"] = 8
        elif scenario == "order-inactive":
            _orders[0]["state"] = 4
        else:
            del _orders[0]["state"]
    elif scenario == "overflow":
        _symbol.update(volume_min=1.0, volume_step=1.0,
                       volume_max=float(2 ** 52))
        _positions = [_position(1000 + index, volume=float(2 ** 52))
                      for index in range(4096)]


def initialize():
    """Allow the bridge to own a real dependency-free interpreter lifecycle."""
    return True


def shutdown():
    """Release the fake broker session without touching its lifetime send audit."""
    return True


def last_error():
    """Supply deterministic diagnostic context expected by runtime initialization."""
    return 1, "Success"


def account_info():
    """Return fresh complete account fields, with optional observed policy change."""
    count = _count("account_info")
    if _scenario == "account-none":
        return None
    result = dict(_account)
    if _scenario == "account-switch" and count > 1:
        result["login"] += 1
    return _record(result)


def terminal_info():
    """Return terminal flags without relying on bool-like integer coercion."""
    _count("terminal_info")
    return _record(dict(_terminal))


def symbol_info(symbol):
    """Enforce the exact requested symbol and optionally change its policy."""
    if symbol != "EURUSD":
        raise ValueError("unexpected or truncated symbol request")
    count = _count("symbol_info")
    result = dict(_symbol)
    if _scenario == "policy-change" and count > 1:
        result["volume_max"] = 50.0
    if _scenario == "late-account-switch" and count > 1:
        _account["login"] += 1
    return _record(result)


def positions_total():
    """Expose account-wide counts, including incomplete and changing cases."""
    count = _count("positions_total")
    if _scenario == "total-none":
        return None
    if _scenario == "total-bool":
        return True
    if _scenario == "oversized-total":
        return 100001
    if _scenario == "count-mismatch":
        return len(_positions) + 1
    if _scenario == "count-change" and count > 1:
        return len(_positions) + 1
    return len(_positions)


def orders_total():
    """Expose a complete unfiltered active-order count."""
    _count("orders_total")
    return len(_orders)


def positions_get():
    """Reject filtering by signature and return independent complete row copies."""
    count = _count("positions_get")
    if _scenario == "python-error":
        raise RuntimeError("deterministic positions failure")
    if _scenario == "slow" and count == 1:
        time.sleep(1.1)
    if _scenario == "positions-none":
        return None
    if _scenario == "positions-generator":
        return (dict(row) for row in _positions)
    result = [dict(row) for row in _positions]
    if _scenario == "replace-position" and count > 1:
        result[0]["ticket"] += 1
    if _scenario == "change-volume" and count > 1:
        result[0]["volume"] = 0.04
    if _scenario == "permuted" and count > 1:
        result.reverse()
    return tuple(_record(row) for row in result)


def orders_get():
    """Return active remainder, including a same-count replacement scenario."""
    count = _count("orders_get")
    if _scenario == "orders-none":
        return None
    result = [dict(row) for row in _orders]
    if _scenario == "replace-order" and count > 1:
        result[0]["ticket"] += 1
    if _scenario == "permuted" and count > 1:
        result.reverse()
    return [_record(row) for row in result]


def order_send(*args, **kwargs):
    """Make any accidental broker dispatch observable and immediately fatal."""
    global _send_calls
    _send_calls += 1
    raise AssertionError("capability observation must never dispatch")


def stats():
    """Return a copy of source-call counts and the non-resettable send counter."""
    return dict(_calls, order_send=_send_calls)


configure("empty")

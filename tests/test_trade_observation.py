"""Deterministic tests for the typed Stage 1 trade observation ABI."""

from __future__ import annotations

import ctypes
import os
import sys
import threading
import types
import unittest
from ctypes import POINTER, Structure, byref, c_char, c_char_p, c_double, c_int, c_int32
from ctypes import c_int64, c_uint8, c_uint32, c_uint64, c_void_p, c_size_t


TEXT_CAPACITY = 128
ACCOUNT_KNOWN_HEDGE_ALLOWED = 1 << 9
SYMBOL_KNOWN_FILLING_MODE = 1 << 3
SYMBOL_KNOWN_VOLUME_LIMITS = 1 << 11
SYMBOL_KNOWN_TRADE_EXEMODE = 1 << 2
ORDER_KNOWN_EXTERNAL_ID = 1 << 21


class Mt5AccountInfo(Structure):
    _fields_ = [
        ("server", c_char * TEXT_CAPACITY),
        ("currency", c_char * 16),
        ("login", c_uint64),
        ("margin_mode", c_int32),
        ("trade_mode", c_int32),
        ("leverage", c_int32),
        ("trade_allowed", c_uint8),
        ("trade_expert", c_uint8),
        ("fifo_close", c_uint8),
        ("hedge_allowed", c_uint8),
        ("balance", c_double),
        ("equity", c_double),
        ("known_fields", c_uint64),
    ]


class Mt5SymbolRequest(Structure):
    _fields_ = [("symbol_utf8", c_char_p), ("reserved", c_uint32)]


class Mt5SymbolCapabilities(Structure):
    _fields_ = [
        ("symbol", c_char * 64),
        ("trade_mode", c_uint32),
        ("trade_exemode", c_uint32),
        ("order_mode", c_uint32),
        ("filling_mode", c_uint32),
        ("expiration_mode", c_uint32),
        ("order_gtc_mode", c_uint32),
        ("trade_stops_level", c_int32),
        ("trade_freeze_level", c_int32),
        ("closeby_allowed", c_uint32),
        ("visible", c_uint32),
        ("selected", c_uint32),
        ("volume_min", c_double),
        ("volume_max", c_double),
        ("volume_step", c_double),
        ("volume_limit", c_double),
        ("trade_tick_size", c_double),
        ("point", c_double),
        ("known_fields", c_uint64),
    ]


class Mt5OrderCheckRequest(Structure):
    _fields_ = [
        ("symbol_utf8", c_char_p),
        ("comment_utf8", c_char_p),
        ("magic", c_uint64),
        ("order", c_uint64),
        ("position", c_uint64),
        ("position_by", c_uint64),
        ("volume", c_double),
        ("price", c_double),
        ("stoplimit", c_double),
        ("sl", c_double),
        ("tp", c_double),
        ("expiration", c_int64),
        ("action", c_uint32),
        ("type", c_uint32),
        ("type_filling", c_uint32),
        ("type_time", c_uint32),
        ("deviation", c_uint32),
        ("reserved", c_uint32 * 3),
    ]


class Mt5OrderCheckResult(Structure):
    _fields_ = [
        ("retcode", c_uint32),
        ("balance", c_double),
        ("equity", c_double),
        ("profit", c_double),
        ("margin", c_double),
        ("margin_free", c_double),
        ("margin_level", c_double),
        ("comment", c_char * TEXT_CAPACITY),
        ("reserved", c_uint32 * 2),
    ]


class Mt5OrdersRequest(Structure):
    _fields_ = [("symbol_utf8", c_char_p), ("group_utf8", c_char_p), ("ticket", c_uint64), ("reserved", c_uint32)]


class Mt5PositionsRequest(Structure):
    _fields_ = [
        ("symbol_utf8", c_char_p), ("group_utf8", c_char_p), ("ticket", c_uint64),
        ("identifier", c_uint64), ("reserved", c_uint32)
    ]


class Mt5HistoryRequest(Structure):
    _fields_ = [
        ("from_msc", c_int64), ("to_msc", c_int64), ("group_utf8", c_char_p),
        ("ticket", c_uint64), ("position_id", c_uint64), ("reserved", c_uint32)
    ]


class Mt5OrderSnapshot(Structure):
    _fields_ = [
        ("ticket", c_uint64), ("position_id", c_uint64), ("position_by_id", c_uint64),
        ("magic", c_uint64), ("type", c_uint32), ("state", c_uint32),
        ("reason", c_uint32), ("type_time", c_uint32), ("type_filling", c_uint32),
        ("volume_initial", c_double), ("volume_current", c_double),
        ("price_open", c_double), ("price_current", c_double), ("price_stoplimit", c_double),
        ("sl", c_double), ("tp", c_double), ("time_setup_msc", c_int64),
        ("time_done_msc", c_int64), ("time_expiration_msc", c_int64),
        ("symbol", c_char * 64), ("comment", c_char * TEXT_CAPACITY),
        ("external_id", c_char * TEXT_CAPACITY), ("known_fields", c_uint64),
        ("reserved", c_uint32 * 2),
    ]


class Mt5PositionSnapshot(Structure):
    _fields_ = [
        ("ticket", c_uint64), ("identifier", c_uint64), ("magic", c_uint64),
        ("type", c_uint32), ("reason", c_uint32), ("volume", c_double),
        ("price_open", c_double), ("price_current", c_double), ("sl", c_double),
        ("tp", c_double), ("profit", c_double), ("swap", c_double),
        ("time_msc", c_int64), ("time_update_msc", c_int64), ("symbol", c_char * 64),
        ("comment", c_char * TEXT_CAPACITY), ("external_id", c_char * TEXT_CAPACITY),
        ("known_fields", c_uint64), ("reserved", c_uint32 * 2),
    ]


class Mt5DealSnapshot(Structure):
    _fields_ = [
        ("ticket", c_uint64), ("order_ticket", c_uint64), ("position_id", c_uint64),
        ("magic", c_uint64), ("type", c_uint32), ("entry", c_uint32), ("reason", c_uint32),
        ("volume", c_double), ("price", c_double), ("profit", c_double),
        ("commission", c_double), ("swap", c_double), ("fee", c_double),
        ("time_msc", c_int64), ("symbol", c_char * 64),
        ("comment", c_char * TEXT_CAPACITY), ("external_id", c_char * TEXT_CAPACITY),
        ("known_fields", c_uint64), ("reserved", c_uint32 * 2),
    ]


def _trade_order(ticket: int = 101) -> dict:
    return {
        "ticket": ticket, "position_id": 201, "position_by_id": 0, "magic": 42,
        "type": 0, "state": 1, "reason": 2, "type_time": 0, "type_filling": 1,
        "volume_initial": 0.5, "volume_current": 0.25, "price_open": 1.1,
        "price_current": 1.2, "price_stoplimit": 0.0, "sl": 1.0, "tp": 1.4,
        "time_setup": 1700000000, "time_done": 1700000001, "time_expiration": 1700003600,
        "symbol": "EURUSD", "comment": "observation", "external_id": "broker-order-1",
    }


def _trade_position() -> dict:
    return {
        "ticket": 301, "identifier": 401, "magic": 42, "type": 0, "reason": 2,
        "volume": 0.25, "price_open": 1.1, "price_current": 1.2, "sl": 1.0,
        "tp": 1.4, "profit": 25.0, "swap": -0.5, "time": 1700000000,
        "time_update": 1700000001, "symbol": "EURUSD", "comment": "position",
        "external_id": "broker-position-1",
    }


def _trade_deal() -> dict:
    return {
        "ticket": 501, "order": 101, "position_id": 401, "magic": 42,
        "type": 0, "entry": 0, "reason": 2, "volume": 0.25, "price": 1.1,
        "profit": 0.0, "commission": -0.2, "swap": 0.0, "fee": 0.0,
        "time": 1700000002, "symbol": "EURUSD", "comment": "deal",
        "external_id": "broker-deal-1",
    }


def fake_module(*, symbol_none: bool = False, order_none: bool = False) -> types.ModuleType:
    """Build a minimal MetaTrader5 module for the external-interpreter path."""
    module = types.ModuleType("MetaTrader5")
    module.SYMBOL_ORDER_CLOSEBY = 64
    module.order_requests: list[dict] = []
    module.collection_requests: list[tuple[str, tuple, dict]] = []

    def initialize() -> bool:
        return True

    def shutdown() -> None:
        return None

    def account_info() -> object:
        return types.SimpleNamespace(
            server="Demo-Trade",
            currency="USD",
            login=4_294_967_297,
            margin_mode=2,
            trade_mode=0,
            leverage=100,
            trade_allowed=True,
            trade_expert=True,
            fifo_close=False,
            balance=10000.5,
            equity=9990.25,
        )

    def symbol_info(symbol: str) -> object:
        if symbol_none:
            return None
        return {
            "name": symbol,
            "trade_mode": 4,
            "trade_exemode": 2,
            "order_mode": 1 | module.SYMBOL_ORDER_CLOSEBY,
            "filling_mode": 3,
            "expiration_mode": 7,
            "order_gtc_mode": 1,
            "trade_stops_level": 12,
            "trade_freeze_level": 3,
            "select": True,
            "visible": True,
            "volume_min": 0.01,
            "volume_max": 100.0,
            "volume_step": 0.01,
            "volume_limit": 200.0,
            "trade_tick_size": 0.00001,
            "point": 0.00001,
        }

    def order_check(request: dict) -> object:
        module.order_requests.append(dict(request))
        if order_none:
            return None
        return {
            "retcode": 10030,
            "balance": 10000.5,
            "equity": 9980.0,
            "profit": -10.25,
            "margin": 125.0,
            "margin_free": 9855.0,
            "margin_level": 7984.0,
            "comment": "invalid filling mode",
        }

    def orders_get(*args: object, **kwargs: object) -> object:
        module.collection_requests.append(("orders_get", args, dict(kwargs)))
        return [_trade_order()]

    def positions_get(*args: object, **kwargs: object) -> object:
        module.collection_requests.append(("positions_get", args, dict(kwargs)))
        return [_trade_position()]

    def history_orders_get(*args: object, **kwargs: object) -> object:
        module.collection_requests.append(("history_orders_get", args, dict(kwargs)))
        return [_trade_order(102)]

    def history_deals_get(*args: object, **kwargs: object) -> object:
        module.collection_requests.append(("history_deals_get", args, dict(kwargs)))
        return [_trade_deal()]

    module.initialize = initialize
    module.shutdown = shutdown
    module.last_error = lambda: (1, "Success")
    module.account_info = account_info
    module.symbol_info = symbol_info
    module.order_check = order_check
    module.orders_get = orders_get
    module.positions_get = positions_get
    module.history_orders_get = history_orders_get
    module.history_deals_get = history_deals_get
    return module


class TradeObservationTests(unittest.TestCase):
    """Exercises typed trade observations without a live terminal."""

    @classmethod
    def setUpClass(cls) -> None:
        if (
            ctypes.sizeof(Mt5AccountInfo) != 192
            or ctypes.sizeof(Mt5SymbolCapabilities) != 168
            or ctypes.sizeof(Mt5HistoryRequest) != 48
            or ctypes.sizeof(Mt5OrderSnapshot) != 472
            or ctypes.sizeof(Mt5PositionSnapshot) != 440
            or ctypes.sizeof(Mt5DealSnapshot) != 440
        ):
            raise unittest.SkipTest("ctypes trade ABI layout does not match the public header")
        path = os.environ.get("MT5BRIDGE_DLL")
        if not path:
            raise unittest.SkipTest("set MT5BRIDGE_DLL to a built mt5_bridge.dll")
        try:
            cls.dll = ctypes.WinDLL(path)
        except OSError as error:
            raise unittest.SkipTest(f"DLL dependencies unavailable: {error}") from error
        cls.dll.mt5bridge_abi_version.restype = c_uint32
        cls.dll.mt5bridge_trade_api_version.restype = c_uint32
        cls.dll.mt5bridge_initialize.argtypes = [ctypes.c_wchar_p]
        cls.dll.mt5bridge_initialize.restype = c_int
        cls.dll.mt5bridge_shutdown.argtypes = []
        cls.dll.mt5bridge_shutdown.restype = c_int
        cls.dll.mt5bridge_last_error.restype = c_char_p
        cls.dll.mt5bridge_account_info.argtypes = [POINTER(Mt5AccountInfo)]
        cls.dll.mt5bridge_account_info.restype = c_int
        cls.dll.mt5bridge_symbol_capabilities.argtypes = [
            POINTER(Mt5SymbolRequest), POINTER(Mt5SymbolCapabilities)
        ]
        cls.dll.mt5bridge_symbol_capabilities.restype = c_int
        cls.dll.mt5bridge_order_check.argtypes = [
            POINTER(Mt5OrderCheckRequest), POINTER(Mt5OrderCheckResult)
        ]
        cls.dll.mt5bridge_order_check.restype = c_int
        cls.dll.mt5bridge_query_orders.argtypes = [POINTER(Mt5OrdersRequest), POINTER(c_void_p)]
        cls.dll.mt5bridge_query_orders.restype = c_int
        cls.dll.mt5bridge_order_buffer_data.argtypes = [c_void_p]
        cls.dll.mt5bridge_order_buffer_data.restype = POINTER(Mt5OrderSnapshot)
        cls.dll.mt5bridge_order_buffer_size.argtypes = [c_void_p]
        cls.dll.mt5bridge_order_buffer_size.restype = c_size_t
        cls.dll.mt5bridge_order_buffer_free.argtypes = [c_void_p]
        cls.dll.mt5bridge_order_buffer_free.restype = None
        cls.dll.mt5bridge_query_positions.argtypes = [POINTER(Mt5PositionsRequest), POINTER(c_void_p)]
        cls.dll.mt5bridge_query_positions.restype = c_int
        cls.dll.mt5bridge_position_buffer_data.argtypes = [c_void_p]
        cls.dll.mt5bridge_position_buffer_data.restype = POINTER(Mt5PositionSnapshot)
        cls.dll.mt5bridge_position_buffer_size.argtypes = [c_void_p]
        cls.dll.mt5bridge_position_buffer_size.restype = c_size_t
        cls.dll.mt5bridge_position_buffer_free.argtypes = [c_void_p]
        cls.dll.mt5bridge_position_buffer_free.restype = None
        cls.dll.mt5bridge_query_history_orders.argtypes = [POINTER(Mt5HistoryRequest), POINTER(c_void_p)]
        cls.dll.mt5bridge_query_history_orders.restype = c_int
        cls.dll.mt5bridge_history_order_buffer_data.argtypes = [c_void_p]
        cls.dll.mt5bridge_history_order_buffer_data.restype = POINTER(Mt5OrderSnapshot)
        cls.dll.mt5bridge_history_order_buffer_size.argtypes = [c_void_p]
        cls.dll.mt5bridge_history_order_buffer_size.restype = c_size_t
        cls.dll.mt5bridge_history_order_buffer_free.argtypes = [c_void_p]
        cls.dll.mt5bridge_history_order_buffer_free.restype = None
        cls.dll.mt5bridge_query_history_deals.argtypes = [POINTER(Mt5HistoryRequest), POINTER(c_void_p)]
        cls.dll.mt5bridge_query_history_deals.restype = c_int
        cls.dll.mt5bridge_deal_buffer_data.argtypes = [c_void_p]
        cls.dll.mt5bridge_deal_buffer_data.restype = POINTER(Mt5DealSnapshot)
        cls.dll.mt5bridge_deal_buffer_size.argtypes = [c_void_p]
        cls.dll.mt5bridge_deal_buffer_size.restype = c_size_t
        cls.dll.mt5bridge_deal_buffer_free.argtypes = [c_void_p]
        cls.dll.mt5bridge_deal_buffer_free.restype = None
        cls.dll.mt5bridge_unsubscribe_all.argtypes = []
        cls.dll.mt5bridge_unsubscribe_all.restype = c_int
        if cls.dll.mt5bridge_abi_version() != 8 or cls.dll.mt5bridge_trade_api_version() != 2:
            raise unittest.SkipTest("test DLL does not expose ABI 8 trade observation")

    def setUp(self) -> None:
        self.fake = fake_module()
        sys.modules["MetaTrader5"] = self.fake
        self.assertEqual(self.dll.mt5bridge_initialize(None), 0, self.last_error())

    def tearDown(self) -> None:
        self.dll.mt5bridge_shutdown()
        sys.modules.pop("MetaTrader5", None)

    def last_error(self) -> str:
        value = self.dll.mt5bridge_last_error()
        return value.decode("utf-8", errors="replace") if value else ""

    def test_account_identity_and_capabilities(self) -> None:
        info = Mt5AccountInfo()
        self.assertEqual(self.dll.mt5bridge_account_info(byref(info)), 0, self.last_error())
        self.assertEqual(info.server, b"Demo-Trade")
        self.assertEqual(info.currency, b"USD")
        self.assertEqual(info.login, 4_294_967_297)
        self.assertEqual(info.margin_mode, 2)
        self.assertEqual(info.hedge_allowed, 0)
        self.assertEqual(info.known_fields & ACCOUNT_KNOWN_HEDGE_ALLOWED, 0)
        self.assertEqual(info.trade_allowed, 1)
        self.assertAlmostEqual(info.equity, 9990.25)

    def test_explicit_hedge_permission_is_distinguished_from_missing_field(self) -> None:
        """A reported false permission is known; an absent field is not."""
        original = self.fake.account_info

        def account_with_explicit_permission() -> object:
            snapshot = original()
            snapshot.hedge_allowed = False
            return snapshot

        self.fake.account_info = account_with_explicit_permission
        info = Mt5AccountInfo()
        self.assertEqual(self.dll.mt5bridge_account_info(byref(info)), 0, self.last_error())
        self.assertEqual(info.hedge_allowed, 0)
        self.assertNotEqual(info.known_fields & ACCOUNT_KNOWN_HEDGE_ALLOWED, 0)

    def test_symbol_capabilities_and_closeby_bit(self) -> None:
        request = Mt5SymbolRequest(b"EURUSD", 0)
        capabilities = Mt5SymbolCapabilities()
        self.assertEqual(
            self.dll.mt5bridge_symbol_capabilities(byref(request), byref(capabilities)),
            0,
            self.last_error(),
        )
        self.assertEqual(capabilities.symbol, b"EURUSD")
        self.assertEqual(capabilities.order_mode, 65)
        self.assertEqual(capabilities.trade_exemode, 2)
        self.assertEqual(capabilities.closeby_allowed, 1)
        self.assertNotEqual(capabilities.known_fields & SYMBOL_KNOWN_TRADE_EXEMODE, 0)
        self.assertEqual(capabilities.selected, 1)
        self.assertAlmostEqual(capabilities.volume_step, 0.01)

    def test_missing_symbol_capabilities_are_reported_as_unknown(self) -> None:
        """Optional zero values never masquerade as supported capabilities."""
        original = self.fake.symbol_info

        def sparse_symbol(symbol: str) -> object:
            snapshot = dict(original(symbol))
            for name in (
                "filling_mode", "expiration_mode", "order_gtc_mode",
                "trade_stops_level", "trade_freeze_level", "visible", "select",
                "volume_min", "volume_max", "volume_step", "volume_limit",
                "trade_tick_size", "point",
            ):
                snapshot.pop(name, None)
            return snapshot

        self.fake.symbol_info = sparse_symbol
        request = Mt5SymbolRequest(b"EURUSD", 0)
        capabilities = Mt5SymbolCapabilities()
        self.assertEqual(
            self.dll.mt5bridge_symbol_capabilities(byref(request), byref(capabilities)),
            0,
            self.last_error(),
        )
        self.assertEqual(capabilities.filling_mode, 0)
        self.assertEqual(capabilities.volume_min, 0.0)
        self.assertEqual(capabilities.volume_max, 0.0)
        self.assertEqual(capabilities.volume_step, 0.0)
        self.assertEqual(capabilities.volume_limit, 0.0)
        self.assertEqual(capabilities.known_fields & SYMBOL_KNOWN_FILLING_MODE, 0)
        self.assertEqual(capabilities.known_fields & SYMBOL_KNOWN_VOLUME_LIMITS, 0)

    def test_order_check_maps_request_and_preserves_rejection(self) -> None:
        request = Mt5OrderCheckRequest(
            b"EURUSD", b"stage1", 42, 7, 8, 9, 0.25, 1.2, 1.1, 1.0, 1.4, 123,
            1, 0, 2, 3, 5, (0, 0, 0)
        )
        result = Mt5OrderCheckResult()
        self.assertEqual(self.dll.mt5bridge_order_check(byref(request), byref(result)), 0)
        self.assertEqual(result.retcode, 10030)
        self.assertEqual(result.comment, b"invalid filling mode")
        self.assertEqual(self.fake.order_requests[-1]["symbol"], "EURUSD")
        self.assertEqual(self.fake.order_requests[-1]["position_by"], 9)
        self.assertEqual(self.fake.order_requests[-1]["action"], 1)
        self.assertAlmostEqual(self.fake.order_requests[-1]["volume"], 0.25)

    def test_incomplete_order_check_result_fails_closed(self) -> None:
        """A missing documented result field must not become an ambiguous zero."""
        def incomplete_order_check(request: dict[str, object]) -> object:
            self.fake.order_requests.append(dict(request))
            return {
                "retcode": 10009,
                "balance": 10000.0,
                "equity": 10000.0,
                "profit": 0.0,
                "margin": 100.0,
                # margin_free is intentionally absent.
                "margin_level": 10000.0,
                "comment": "accepted",
            }

        self.fake.order_check = incomplete_order_check
        result = Mt5OrderCheckResult()
        self.assertNotEqual(
            self.dll.mt5bridge_order_check(byref(Mt5OrderCheckRequest()), byref(result)),
            0,
        )
        self.assertIn("margin_free is required", self.last_error())

    def test_order_check_does_not_hold_lifecycle_mutex_during_python_call(self) -> None:
        """State-only operations remain responsive while MT5 IPC is blocked."""
        entered = threading.Event()
        release = threading.Event()
        original = self.fake.order_check

        def blocking_order_check(request: dict[str, object]) -> object:
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test release timeout")
            return original(request)

        self.fake.order_check = blocking_order_check
        result = Mt5OrderCheckResult()
        call_status: list[int] = []

        def invoke_order_check() -> None:
            call_status.append(
                self.dll.mt5bridge_order_check(
                    byref(Mt5OrderCheckRequest()), byref(result)
                )
            )

        worker = threading.Thread(target=invoke_order_check)
        worker.start()
        state_probe_done = threading.Event()
        state_probe_status: list[int] = []

        def probe_state_mutex() -> None:
            state_probe_status.append(self.dll.mt5bridge_unsubscribe_all())
            state_probe_done.set()

        probe = threading.Thread(target=probe_state_mutex)
        probe_started = False
        try:
            self.assertTrue(entered.wait(2), "fake order_check did not start")
            probe.start()
            probe_started = True
            self.assertTrue(
                state_probe_done.wait(1),
                "state-only operation was blocked by a slow Python call",
            )
        finally:
            release.set()
            worker.join(5)
            if probe_started:
                probe.join(5)
        self.assertFalse(worker.is_alive())
        self.assertFalse(probe.is_alive())
        self.assertEqual(call_status, [0])
        self.assertEqual(state_probe_status, [0])

    def test_shutdown_waits_for_inflight_python_call(self) -> None:
        """Finalization must not race a Python/MT5 call already in progress."""
        entered = threading.Event()
        release = threading.Event()
        original = self.fake.order_check

        def blocking_order_check(request: dict[str, object]) -> object:
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test release timeout")
            return original(request)

        self.fake.order_check = blocking_order_check
        result = Mt5OrderCheckResult()
        call_status: list[int] = []

        def invoke_order_check() -> None:
            call_status.append(
                self.dll.mt5bridge_order_check(
                    byref(Mt5OrderCheckRequest()), byref(result)
                )
            )

        worker = threading.Thread(target=invoke_order_check)
        worker.start()
        shutdown_done = threading.Event()
        shutdown_status: list[int] = []

        def invoke_shutdown() -> None:
            shutdown_status.append(self.dll.mt5bridge_shutdown())
            shutdown_done.set()

        shutdown_thread = threading.Thread(target=invoke_shutdown)
        shutdown_started = False
        try:
            self.assertTrue(entered.wait(2), "fake order_check did not start")
            shutdown_thread.start()
            shutdown_started = True
            self.assertFalse(
                shutdown_done.wait(0.5),
                "shutdown finalized while a Python call was still in flight",
            )
        finally:
            release.set()
            worker.join(5)
            if shutdown_started:
                shutdown_thread.join(5)
        self.assertFalse(worker.is_alive())
        self.assertFalse(shutdown_thread.is_alive())
        self.assertEqual(call_status, [0])
        self.assertEqual(shutdown_status, [0])

        # setUp/tearDown owns one lifecycle per test; restore it after the
        # explicit barrier check so tearDown remains idempotent.
        self.assertEqual(self.dll.mt5bridge_initialize(None), 0, self.last_error())

    def _read_buffer(self, query, data, size, release, request, snapshot_type):
        buffer = c_void_p()
        self.assertEqual(query(byref(request), byref(buffer)), 0, self.last_error())
        try:
            count = size(buffer)
            pointer = data(buffer)
            values = []
            for index in range(count):
                value = snapshot_type()
                ctypes.memmove(byref(value), ctypes.addressof(pointer[index]), ctypes.sizeof(value))
                values.append(value)
            return values
        finally:
            release(buffer)

    def test_typed_active_and_history_snapshots_preserve_graph_fields(self) -> None:
        symbol = c_char_p(b"EURUSD")
        group = c_char_p(b"*EUR*")
        order_request = Mt5OrdersRequest(symbol, group, 101, 0)
        orders = self._read_buffer(
            self.dll.mt5bridge_query_orders,
            self.dll.mt5bridge_order_buffer_data,
            self.dll.mt5bridge_order_buffer_size,
            self.dll.mt5bridge_order_buffer_free,
            order_request,
            Mt5OrderSnapshot,
        )
        self.assertEqual(len(orders), 1)
        self.assertEqual(orders[0].ticket, 101)
        self.assertEqual(orders[0].time_setup_msc, 1_700_000_000_000)
        self.assertEqual(orders[0].external_id, b"broker-order-1")
        self.assertNotEqual(orders[0].known_fields, 0)
        self.assertEqual(self.fake.collection_requests[-1][0], "orders_get")
        self.assertEqual(self.fake.collection_requests[-1][2], {"symbol": "EURUSD", "group": "*EUR*", "ticket": 101})

        position_request = Mt5PositionsRequest(symbol, group, 301, 401, 0)
        positions = self._read_buffer(
            self.dll.mt5bridge_query_positions,
            self.dll.mt5bridge_position_buffer_data,
            self.dll.mt5bridge_position_buffer_size,
            self.dll.mt5bridge_position_buffer_free,
            position_request,
            Mt5PositionSnapshot,
        )
        self.assertEqual(positions[0].identifier, 401)
        self.assertEqual(positions[0].time_update_msc, 1_700_000_001_000)
        self.assertEqual(self.fake.collection_requests[-1][2]["position"], 401)

        history_request = Mt5HistoryRequest(1_700_000_000_000, 1_700_000_010_000, group, 102, 401, 0)
        history_orders = self._read_buffer(
            self.dll.mt5bridge_query_history_orders,
            self.dll.mt5bridge_history_order_buffer_data,
            self.dll.mt5bridge_history_order_buffer_size,
            self.dll.mt5bridge_history_order_buffer_free,
            history_request,
            Mt5OrderSnapshot,
        )
        self.assertEqual(history_orders[0].ticket, 102)
        history_call = self.fake.collection_requests[-1]
        self.assertEqual(history_call[0], "history_orders_get")
        self.assertEqual(history_call[2], {"group": "*EUR*", "ticket": 102, "position": 401})
        self.assertEqual(history_call[1][0].timestamp(), 1_700_000_000)

        history_deals = self._read_buffer(
            self.dll.mt5bridge_query_history_deals,
            self.dll.mt5bridge_deal_buffer_data,
            self.dll.mt5bridge_deal_buffer_size,
            self.dll.mt5bridge_deal_buffer_free,
            history_request,
            Mt5DealSnapshot,
        )
        self.assertEqual(history_deals[0].order_ticket, 101)
        self.assertEqual(history_deals[0].position_id, 401)
        self.assertEqual(history_deals[0].time_msc, 1_700_000_002_000)
        self.assertEqual(history_deals[0].external_id, b"broker-deal-1")

    def test_snapshot_missing_required_graph_field_fails_closed(self) -> None:
        def incomplete_orders_get(*args: object, **kwargs: object) -> object:
            value = _trade_order()
            value.pop("position_id")
            return [value]

        self.fake.orders_get = incomplete_orders_get
        request = Mt5OrdersRequest()
        buffer = c_void_p()
        self.assertNotEqual(self.dll.mt5bridge_query_orders(byref(request), byref(buffer)), 0)
        self.assertIn("position_id is required", self.last_error())

    def test_optional_external_id_remains_unknown(self) -> None:
        def order_without_external_id(*args: object, **kwargs: object) -> object:
            value = _trade_order()
            value.pop("external_id")
            return [value]

        self.fake.orders_get = order_without_external_id
        request = Mt5OrdersRequest()
        orders = self._read_buffer(
            self.dll.mt5bridge_query_orders,
            self.dll.mt5bridge_order_buffer_data,
            self.dll.mt5bridge_order_buffer_size,
            self.dll.mt5bridge_order_buffer_free,
            request,
            Mt5OrderSnapshot,
        )
        self.assertEqual(orders[0].external_id, b"")
        self.assertEqual(orders[0].known_fields & ORDER_KNOWN_EXTERNAL_ID, 0)

    def test_none_collection_with_error_fails_closed(self) -> None:
        self.fake.orders_get = lambda **kwargs: None
        self.fake.last_error = lambda: (2, "trade error")
        request = Mt5OrdersRequest()
        buffer = c_void_p()
        self.assertNotEqual(self.dll.mt5bridge_query_orders(byref(request), byref(buffer)), 0)
        self.assertIn("collection query failed", self.last_error())

    def test_empty_snapshot_is_successful(self) -> None:
        self.fake.positions_get = lambda **kwargs: []
        request = Mt5PositionsRequest()
        values = self._read_buffer(
            self.dll.mt5bridge_query_positions,
            self.dll.mt5bridge_position_buffer_data,
            self.dll.mt5bridge_position_buffer_size,
            self.dll.mt5bridge_position_buffer_free,
            request,
            Mt5PositionSnapshot,
        )
        self.assertEqual(values, [])

    def test_invalid_reserved_field_fails_closed(self) -> None:
        request = Mt5SymbolRequest(b"EURUSD", 1)
        capabilities = Mt5SymbolCapabilities()
        self.assertNotEqual(
            self.dll.mt5bridge_symbol_capabilities(byref(request), byref(capabilities)), 0
        )
        self.assertIn("reserved", self.last_error())

        order = Mt5OrderCheckRequest()
        order.reserved[0] = 1
        result = Mt5OrderCheckResult()
        self.assertNotEqual(self.dll.mt5bridge_order_check(byref(order), byref(result)), 0)
        self.assertIn("reserved", self.last_error())

    def test_none_symbol_and_order_results_fail(self) -> None:
        sys.modules["MetaTrader5"] = fake_module(symbol_none=True)
        self.dll.mt5bridge_shutdown()
        self.assertEqual(self.dll.mt5bridge_initialize(None), 0)
        request = Mt5SymbolRequest(b"MISSING", 0)
        capabilities = Mt5SymbolCapabilities()
        self.assertNotEqual(
            self.dll.mt5bridge_symbol_capabilities(byref(request), byref(capabilities)), 0
        )
        self.assertIn("no snapshot", self.last_error())
        self.dll.mt5bridge_shutdown()

        sys.modules["MetaTrader5"] = fake_module(order_none=True)
        self.assertEqual(self.dll.mt5bridge_initialize(None), 0)
        order = Mt5OrderCheckRequest()
        result = Mt5OrderCheckResult()
        self.assertNotEqual(self.dll.mt5bridge_order_check(byref(order), byref(result)), 0)
        self.assertIn("no result", self.last_error())

    def test_runtime_not_initialized_fails(self) -> None:
        self.dll.mt5bridge_shutdown()
        info = Mt5AccountInfo()
        self.assertNotEqual(self.dll.mt5bridge_account_info(byref(info)), 0)
        self.assertIn("not initialized", self.last_error())


if __name__ == "__main__":
    unittest.main()

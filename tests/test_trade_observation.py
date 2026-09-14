"""Deterministic tests for the typed Stage 1 trade observation ABI."""

from __future__ import annotations

import ctypes
import os
import sys
import types
import unittest
from ctypes import POINTER, Structure, byref, c_char, c_char_p, c_double, c_int, c_int32
from ctypes import c_int64, c_uint8, c_uint32, c_uint64, c_void_p


TEXT_CAPACITY = 128
ACCOUNT_KNOWN_HEDGE_ALLOWED = 1 << 9
SYMBOL_KNOWN_FILLING_MODE = 1 << 3
SYMBOL_KNOWN_VOLUME_LIMITS = 1 << 11
SYMBOL_KNOWN_TRADE_EXEMODE = 1 << 2


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


def fake_module(*, symbol_none: bool = False, order_none: bool = False) -> types.ModuleType:
    """Build a minimal MetaTrader5 module for the external-interpreter path."""
    module = types.ModuleType("MetaTrader5")
    module.SYMBOL_ORDER_CLOSEBY = 64
    module.order_requests: list[dict] = []

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

    module.initialize = initialize
    module.shutdown = shutdown
    module.last_error = lambda: (1, "Success")
    module.account_info = account_info
    module.symbol_info = symbol_info
    module.order_check = order_check
    return module


class TradeObservationTests(unittest.TestCase):
    """Exercises typed trade observations without a live terminal."""

    @classmethod
    def setUpClass(cls) -> None:
        if ctypes.sizeof(Mt5AccountInfo) != 192 or ctypes.sizeof(Mt5SymbolCapabilities) != 168:
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
        if cls.dll.mt5bridge_abi_version() != 8 or cls.dll.mt5bridge_trade_api_version() != 1:
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

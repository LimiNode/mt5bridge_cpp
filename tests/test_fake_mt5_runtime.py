## \file test_fake_mt5_runtime.py
#  \brief Exercises recovery and pagination through a deterministic fake MT5 module.

"""Adversarial smoke tests for the CPython-backed bridge runtime.

Run after building the DLL with the same Python version as the test process:

    $env:MT5BRIDGE_DLL = "...\\mt5_bridge.dll"
    python -m unittest tests.test_fake_mt5_runtime
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
import types
import unittest
from collections import namedtuple
from ctypes import POINTER, Structure, byref, c_char_p, c_double, c_int, c_int32
from ctypes import c_int64, c_size_t, c_uint32, c_uint64, c_void_p

import numpy as np

MT5_GAP_SOURCE_INCONSISTENCY = 2
MT5_SUBSCRIPTION_TICK_BATCH = 0
MT5_SUBSCRIPTION_GAP = 2


class Mt5TicksRequest(Structure):
    """Matches the versioned C ABI tick request."""

    _fields_ = [
        ("symbol_utf8", c_char_p),
        ("from_msc", c_int64),
        ("to_msc", c_int64),
        ("flags", c_uint32),
        ("reserved", c_uint32),
    ]


class Mt5RatesRequest(Structure):
    """Matches the versioned C ABI rate request."""

    _fields_ = [
        ("symbol_utf8", c_char_p),
        ("from_msc", c_int64),
        ("to_msc", c_int64),
        ("timeframe", c_int32),
        ("reserved", c_int32),
    ]


class Mt5FetchDiagnostics(Structure):
    """Matches the versioned C ABI diagnostics record."""

    _fields_ = [
        ("attempts", c_uint32),
        ("retries", c_uint32),
        ("reconnects", c_uint32),
        ("last_mt5_error", c_int32),
        ("history_warmup_detected", ctypes.c_uint8),
        ("complete", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 2),
        ("status", c_int32),
    ]


class Mt5Tick(Structure):
    """Matches the versioned C ABI tick record."""

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


class Mt5TickSourceRequest(Structure):
    """Matches one ABI 8 physical source request."""

    _fields_ = [("symbol_utf8", c_char_p), ("flags", c_uint32), ("reserved", c_uint32)]


class Mt5SubscriptionRequest(Structure):
    """Matches the ABI 8 multi-source realtime subscription request."""

    _fields_ = [
        ("sources", POINTER(Mt5TickSourceRequest)),
        ("source_count", c_size_t),
        ("interval_ms", c_uint32),
        ("max_batch", c_uint32),
        ("ring_capacity", c_uint32),
        ("delivery_flags", c_uint32),
        ("stale_after_ms", c_uint32),
        ("reserved", c_uint32 * 2),
    ]


class Mt5SubscriptionHandle(Structure):
    """Matches the ABI 8 generation-qualified handle."""

    _fields_ = [("generation", c_uint64), ("id", c_uint64)]


class Mt5SubscriptionEvent(Structure):
    """Matches the borrowed realtime event view."""

    _fields_ = [
        ("type", c_int32),
        ("status", c_int32),
        ("handle", Mt5SubscriptionHandle),
        ("sequence", c_uint64),
        ("source_index", c_uint32),
        ("ticks", c_void_p),
        ("count", c_size_t),
        ("dropped", c_uint64),
        ("gap_reason", c_int32),
        ("recovery_from_msc", c_int64),
        ("recovery_to_msc", c_int64),
        ("snapshot", c_void_p),
    ]


class Mt5SubscriptionDiagnostics(Structure):
    """Matches per-source realtime diagnostics."""

    _fields_ = [
        ("last_mt5_error", c_int32),
        ("reconnects", c_uint32),
        ("consecutive_failures", c_uint32),
        ("history_lag_ms", c_int64),
        ("poll_duration_us", c_uint64),
        ("history_rewrites", c_uint32),
        ("gap_reason", c_uint32),
        ("status", c_int32),
    ]


## \brief Structured NumPy layout returned by the MetaTrader5 Python package.
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


def page(start: int, count: int) -> np.ndarray:
    """Builds deterministic ticks with tied timestamps at the page boundary."""
    values = np.zeros(count, dtype=TICK_DTYPE)
    values["time"] = start // 1000
    values["time_msc"] = start
    values["bid"] = 1.1
    values["ask"] = 1.2
    values["last"] = 1.15
    values["volume"] = np.arange(count, dtype=np.uint64)
    values["volume_real"] = values["volume"] / 10.0
    values["flags"] = 1
    return values


def page_with_tail() -> np.ndarray:
    """Builds a repeated boundary page followed by two newer ticks."""
    values = page(2000, 65538)
    values[65536:]["time"] = 2
    values[65536:]["time_msc"] = (2500, 2501)
    return values


RATE_DTYPE = np.dtype(
    [
        ("time", "<i8"),
        ("open", "<f8"),
        ("high", "<f8"),
        ("low", "<f8"),
        ("close", "<f8"),
        ("tick_volume", "<i8"),
        ("spread", "<i4"),
        ("real_volume", "<i8"),
    ]
)


def rate_page(count: int) -> np.ndarray:
    """Builds deterministic OHLC rows for rates recovery tests."""
    values = np.zeros(count, dtype=RATE_DTYPE)
    values["time"] = np.arange(count, dtype=np.int64)
    values["open"] = 1.1
    values["high"] = 1.2
    values["low"] = 1.0
    values["close"] = 1.15
    values["tick_volume"] = 10
    values["spread"] = 2
    values["real_volume"] = 20
    return values


def fake_module(
    sequence: list[object], *, order_error: BaseException | None = None,
    rate_sequence: list[object] | None = None, initialize_result: bool = True,
    histories: dict[str, np.ndarray] | None = None, order_none: bool = False,
    history_sequence: list[object] | None = None,
    order_response: object | None = None, account_server: str = "Fake-Server",
    account_login: int = 42,
) -> types.ModuleType:
    """Creates a fake MetaTrader5 module consuming a scripted sequence."""
    module = types.ModuleType("MetaTrader5")
    module.COPY_TICKS_ALL = 3
    module.TRADE_ACTION_DEAL = 1
    module.ORDER_TYPE_BUY = 0
    module.calls = 0
    module.request_counts: list[int] = []
    module.request_starts: list[int] = []
    module.initialize_calls = 0
    module.shutdown_calls = 0
    module.order_calls = 0
    module.call_order: list[str] = []
    module.account_server = account_server
    module.account_login = account_login
    module.last = (1, "Success")
    module.histories = histories or {}
    module.history_sequence = list(history_sequence or [])

    def initialize() -> bool:
        module.initialize_calls += 1
        return initialize_result

    def shutdown() -> bool:
        module.shutdown_calls += 1
        return True

    def last_error() -> tuple[int, str]:
        return module.last

    def copy_ticks_from(symbol: str, when: object, count: int, flags: int) -> object:
        del flags
        module.calls += 1
        module.request_counts.append(count)
        module.request_starts.append(int(when.timestamp() * 1000))
        if module.history_sequence:
            result = module.history_sequence.pop(0)
        elif symbol in module.histories:
            values = module.histories[symbol]
            start_msc = int(when.timestamp() * 1000)
            start = int(np.searchsorted(values["time_msc"], start_msc, side="left"))
            result = values[start : start + count]
        else:
            result = sequence.pop(0) if sequence else page(2000, 0)
        if callable(result):
            try:
                result = result(symbol, when, count)
            except TypeError:
                try:
                    result = result(symbol, when)
                except TypeError:
                    try:
                        result = result(when)
                    except TypeError:
                        result = result()
        if isinstance(result, tuple):
            result, code, message = result
            module.last = (code, message)
            if isinstance(result, np.ndarray):
                result = result[:count]
        elif result is None:
            module.last = (-4, "History timeout")
        else:
            if isinstance(result, np.ndarray):
                result = result[:count]
            module.last = (1, "Success")
        return result

    def order_send(request: dict[str, object]) -> dict[str, object] | None:
        if request.get("action") != module.TRADE_ACTION_DEAL or request.get("type") != module.ORDER_TYPE_BUY:
            raise ValueError("invalid market order request")
        module.order_calls += 1
        module.call_order.append("order_send")
        if order_error is not None:
            raise order_error
        if order_none:
            return None
        if order_response is not None:
            return order_response() if callable(order_response) else order_response
        return {"retcode": 10009}

    def account_info() -> dict[str, object]:
        module.call_order.append("account_info")
        return {"server": module.account_server, "login": module.account_login}

    def terminal_info() -> dict[str, bool]:
        return {"connected": True}

    def copy_rates_range(symbol: str, timeframe: int, start: object, end: object) -> object:
        del symbol, timeframe, start, end
        result = rate_sequence.pop(0) if rate_sequence else rate_page(0)
        if isinstance(result, tuple):
            result, code, message = result
            module.last = (code, message)
        elif result is None:
            module.last = (-4, "History timeout")
        else:
            module.last = (1, "Success")
        return result

    module.initialize = initialize
    module.shutdown = shutdown
    module.last_error = last_error
    module.copy_ticks_from = copy_ticks_from
    module.order_send = order_send
    module.account_info = account_info
    module.terminal_info = terminal_info
    if rate_sequence is not None:
        module.copy_rates_range = copy_rates_range
    return module


_FAKE_TRADE_REQUEST = namedtuple(
    "FakeTradeRequest",
    [
        "action",
        "magic",
        "order",
        "symbol",
        "volume",
        "price",
        "stoplimit",
        "sl",
        "tp",
        "deviation",
        "type",
        "type_filling",
        "type_time",
        "expiration",
        "comment",
        "position",
        "position_by",
    ],
)

_FAKE_TRADE_RESULT = namedtuple(
    "FakeTradeResult",
    [
        "retcode",
        "deal",
        "order",
        "volume",
        "price",
        "bid",
        "ask",
        "comment",
        "request_id",
        "retcode_external",
        "request",
    ],
)


def valid_order_result(
    retcode: int, *, namedtuple_result: bool = False, retcode_external: int = 0
) -> dict[str, object] | object:
    """Builds the complete MqlTradeResult-shaped payload required by the adapter."""
    values: dict[str, object] = {
        "retcode": retcode,
        "deal": 0,
        "order": 0,
        "volume": 0.01,
        "price": 1.1,
        "bid": 1.099,
        "ask": 1.101,
        "comment": "Fake result",
        "request_id": 7,
        "retcode_external": retcode_external,
        "request": {
            "action": 1,
            "magic": 123,
            "symbol": "EURUSD",
            "volume": 0.01,
        },
    }
    if namedtuple_result:
        values["request"] = _FAKE_TRADE_REQUEST(
            1,
            123,
            0,
            "EURUSD",
            0.01,
            1.1,
            0.0,
            0.0,
            0.0,
            10,
            0,
            0,
            0,
            0,
            "",
            0,
            0,
        )
        return _FAKE_TRADE_RESULT(**values)
    return values


class FakeMt5RuntimeTests(unittest.TestCase):
    """Verifies retry, page-boundary, and no-progress behavior without MT5."""

    @classmethod
    def setUpClass(cls) -> None:
        """Loads the DLL and installs the deterministic fake module."""
        if ctypes.sizeof(Mt5TicksRequest) != 32 or ctypes.sizeof(Mt5Tick) != 56:
            raise unittest.SkipTest("ctypes ABI layout does not match ABI version 8")
        dll_path = os.environ.get("MT5BRIDGE_DLL")
        if not dll_path:
            raise unittest.SkipTest("set MT5BRIDGE_DLL to a built mt5_bridge.dll")
        try:
            cls.module = ctypes.WinDLL(dll_path)
        except OSError as error:
            raise unittest.SkipTest(
                f"DLL dependencies are unavailable (build/test Python mismatch?): {error}"
            ) from error
        cls.module.mt5bridge_abi_version.restype = c_uint32
        cls.module.mt5bridge_initialize.argtypes = [ctypes.c_wchar_p]
        cls.module.mt5bridge_initialize.restype = c_int
        cls.module.mt5bridge_shutdown.argtypes = []
        cls.module.mt5bridge_shutdown.restype = c_int
        cls.module.mt5bridge_last_error.argtypes = []
        cls.module.mt5bridge_last_error.restype = c_char_p
        cls.module.mt5bridge_eval_json.argtypes = [c_char_p, POINTER(c_void_p)]
        cls.module.mt5bridge_eval_json.restype = c_int
        cls.module.mt5bridge_free.argtypes = [c_void_p]
        cls.module.mt5bridge_free.restype = None
        cls.module.mt5bridge_query_ticks.argtypes = [POINTER(Mt5TicksRequest), POINTER(c_void_p)]
        cls.module.mt5bridge_query_ticks.restype = c_int
        cls.module.mt5bridge_tick_buffer_size.argtypes = [c_void_p]
        cls.module.mt5bridge_tick_buffer_size.restype = c_size_t
        cls.module.mt5bridge_tick_buffer_diagnostics.argtypes = [
            c_void_p,
            POINTER(Mt5FetchDiagnostics),
        ]
        cls.module.mt5bridge_tick_buffer_free.argtypes = [c_void_p]
        cls.module.mt5bridge_query_rates.argtypes = [POINTER(Mt5RatesRequest), POINTER(c_void_p)]
        cls.module.mt5bridge_query_rates.restype = c_int
        cls.module.mt5bridge_rate_buffer_size.argtypes = [c_void_p]
        cls.module.mt5bridge_rate_buffer_size.restype = c_size_t
        cls.module.mt5bridge_rate_buffer_diagnostics.argtypes = [
            c_void_p,
            POINTER(Mt5FetchDiagnostics),
        ]
        cls.module.mt5bridge_rate_buffer_free.argtypes = [c_void_p]
        cls.module.mt5bridge_last_fetch_diagnostics.argtypes = [POINTER(Mt5FetchDiagnostics)]
        cls.module.mt5bridge_last_fetch_diagnostics.restype = c_int
        cls.tick_callback_type = ctypes.CFUNCTYPE(
            c_int, POINTER(Mt5Tick), c_size_t, c_void_p
        )
        cls.module.mt5bridge_copy_ticks_range.argtypes = [
            POINTER(Mt5TicksRequest),
            c_size_t,
            cls.tick_callback_type,
            c_void_p,
        ]
        cls.module.mt5bridge_copy_ticks_range.restype = c_int
        cls.module.mt5bridge_subscribe_ticks.argtypes = [
            POINTER(Mt5SubscriptionRequest), POINTER(Mt5SubscriptionHandle)
        ]
        cls.module.mt5bridge_subscribe_ticks.restype = c_int
        cls.module.mt5bridge_unsubscribe.argtypes = [Mt5SubscriptionHandle]
        cls.module.mt5bridge_unsubscribe.restype = c_int
        cls.event_callback_type = ctypes.CFUNCTYPE(c_int, POINTER(Mt5SubscriptionEvent), c_void_p)
        cls.module.mt5bridge_process_events.argtypes = [c_size_t, cls.event_callback_type, c_void_p]
        cls.module.mt5bridge_process_events.restype = c_int
        cls.module.mt5bridge_subscription_source_diagnostics.argtypes = [
            Mt5SubscriptionHandle, c_uint32, POINTER(Mt5SubscriptionDiagnostics)
        ]
        cls.module.mt5bridge_subscription_source_diagnostics.restype = c_int
        if cls.module.mt5bridge_abi_version() != 8:
            raise unittest.SkipTest("test DLL does not expose ABI version 8")

    def tearDown(self) -> None:
        """Restores the module registry after each scenario."""
        sys.modules.pop("MetaTrader5", None)

    def last_error(self) -> str:
        """Returns the bridge diagnostic for an assertion message."""
        value = self.module.mt5bridge_last_error()
        return value.decode("utf-8", errors="replace") if value else ""

    def dispatch(self, fake: types.ModuleType) -> tuple[int, dict[str, object], str]:
        """Exercises the test-only private Python dispatch adapter hook."""
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        response = c_void_p()
        status = self.module.mt5bridge_eval_json(
            json.dumps({"method": "test_dispatch_transport"}).encode("utf-8"),
            byref(response),
        )
        payload: dict[str, object] = {}
        if response.value:
            payload = json.loads(ctypes.string_at(response.value).decode("utf-8"))
            self.module.mt5bridge_free(response)
        error = self.last_error()
        self.module.mt5bridge_shutdown()
        return status, payload, error

    def test_private_dispatch_transport_persists_market_closed_result(self) -> None:
        """A complete 10018 result is broker evidence and is classified rejected."""
        fake = fake_module([], order_response=valid_order_result(10018))
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 1)
        self.assertEqual(fake.call_order, ["account_info", "order_send"])
        self.assertEqual(payload["status"], "broker_result")
        self.assertEqual(payload["retcode"], 10018)
        self.assertEqual(payload["disposition"], "rejected")
        self.assertEqual(payload["raw_result"], valid_order_result(10018))

    def test_private_dispatch_transport_seeds_reconciliation_for_success(self) -> None:
        """Successful or partial results seed reconciliation instead of final fill."""
        for retcode in (10009, 10008):
            with self.subTest(retcode=retcode):
                fake = fake_module([], order_response=valid_order_result(retcode))
                status, payload, error = self.dispatch(fake)
                self.assertEqual(status, 0, error)
                self.assertEqual(fake.order_calls, 1)
                self.assertEqual(payload["status"], "broker_result")
                self.assertEqual(payload["retcode"], retcode)
                self.assertEqual(payload["disposition"], "reconciling")

    def test_private_dispatch_transport_accepts_namedtuple_result(self) -> None:
        """The real MetaTrader5 namedtuple result is normalized to JSON fields."""
        fake = fake_module(
            [], order_response=valid_order_result(10009, namedtuple_result=True)
        )
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 1)
        self.assertEqual(payload["status"], "broker_result")
        self.assertEqual(payload["retcode"], 10009)
        self.assertEqual(payload["raw_result"]["request"]["action"], 1)
        self.assertEqual(payload["raw_result"]["request"]["symbol"], "EURUSD")
        self.assertEqual(payload["raw_result"]["request"]["position_by"], 0)

    def test_private_dispatch_transport_accepts_signed_external_retcode(self) -> None:
        """Signed external broker codes remain valid result evidence."""
        fake = fake_module([], order_response=valid_order_result(10009, retcode_external=-7))
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(payload["status"], "broker_result")
        self.assertEqual(payload["raw_result"]["retcode_external"], -7)

    def test_private_dispatch_transport_keeps_locked_request_reconciling(self) -> None:
        """A locked request is not strong enough to become terminal rejected."""
        fake = fake_module([], order_response=valid_order_result(10028))
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(payload["status"], "broker_result")
        self.assertEqual(payload["disposition"], "reconciling")

    def test_private_dispatch_transport_exception_is_unresolved(self) -> None:
        """A Python exception after the one call is not broker evidence."""
        fake = fake_module([], order_error=RuntimeError("send failed"))
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 1)
        self.assertEqual(payload["status"], "transport_failure")
        self.assertEqual(payload["retcode"], 0)
        self.assertIsNone(payload["raw_result"])

    def test_private_dispatch_transport_none_result_is_unresolved(self) -> None:
        """A None return cannot be persisted as a trustworthy broker result."""
        fake = fake_module([], order_none=True)
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 1)
        self.assertEqual(payload["status"], "transport_failure")
        self.assertIsNone(payload["raw_result"])

    def test_private_dispatch_transport_malformed_result_is_unresolved(self) -> None:
        """A partial result shape is rejected before durable serialization."""
        fake = fake_module([], order_response={"retcode": 10018})
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 1)
        self.assertEqual(payload["status"], "transport_failure")
        self.assertEqual(payload["retcode"], 0)
        self.assertIsNone(payload["raw_result"])

    def test_private_dispatch_transport_account_mismatch_skips_send(self) -> None:
        """The adapter's final account check prevents a send on another account."""
        fake = fake_module(
            [], order_response=valid_order_result(10018), account_server="Other-Server"
        )
        status, payload, error = self.dispatch(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(fake.order_calls, 0)
        self.assertEqual(fake.call_order, ["account_info"])
        self.assertEqual(payload["status"], "account_mismatch")
        self.assertEqual(payload["retcode"], 0)
        self.assertIsNone(payload["raw_result"])

    def query(
        self, fake: types.ModuleType
    ) -> tuple[int, int, Mt5FetchDiagnostics, str]:
        """Runs one tick query and returns status, size, diagnostics, and error."""
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        request = Mt5TicksRequest(b"EURUSD", 1000, 3000, 0)
        result = c_void_p()
        status = self.module.mt5bridge_query_ticks(byref(request), byref(result))
        diagnostics = Mt5FetchDiagnostics()
        size = 0
        if result.value:
            size = self.module.mt5bridge_tick_buffer_size(result)
            self.module.mt5bridge_tick_buffer_diagnostics(result, byref(diagnostics))
            self.module.mt5bridge_tick_buffer_free(result)
        else:
            self.module.mt5bridge_last_fetch_diagnostics(byref(diagnostics))
        error = self.last_error()
        self.module.mt5bridge_shutdown()
        return status, size, diagnostics, error

    def query_rates(
        self, fake: types.ModuleType
    ) -> tuple[int, int, Mt5FetchDiagnostics, str]:
        """Runs one rate query and returns status, size, diagnostics, and error."""
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        native_request = Mt5RatesRequest(b"EURUSD", 0, 3000, 1, 0)
        result = c_void_p()
        status = self.module.mt5bridge_query_rates(byref(native_request), byref(result))
        diagnostics = Mt5FetchDiagnostics()
        size = 0
        if result.value:
            size = self.module.mt5bridge_rate_buffer_size(result)
            self.module.mt5bridge_rate_buffer_diagnostics(result, byref(diagnostics))
            self.module.mt5bridge_rate_buffer_free(result)
        error = self.last_error()
        self.module.mt5bridge_shutdown()
        return status, size, diagnostics, error

    def test_retry_then_empty_is_success(self) -> None:
        """A transient None followed by an empty page is not a fatal error."""
        status, size, diagnostics, error = self.query(
            fake_module([None, page(2000, 0)])
        )
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 0)
        self.assertEqual(diagnostics.retries, 1)
        self.assertTrue(diagnostics.history_warmup_detected)

    def test_tied_timestamp_boundary_is_lossless(self) -> None:
        """A full page of tied timestamps is followed without loss or duplicates."""
        status, size, _, error = self.query(
            fake_module([page(2000, 65536), page_with_tail()])
        )
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 65538)

    def test_same_timestamp_reordering_between_pages_is_lossless(self) -> None:
        """Boundary pagination matches payloads instead of relying on row order."""
        first = page(2000, 65536)
        second = page(2000, 65537)
        second = np.concatenate((second[1:], second[:1]))
        fake = fake_module([first, second])
        status, size, _, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 65537)

    def test_three_reads_of_same_timestamp_preserve_full_multiplicity(self) -> None:
        """Persistent boundary multiplicity survives repeated reordered rereads."""
        first = page(2000, 65536)
        second = page(2000, 65537)
        second = np.concatenate((second[32768:], second[:32768]))
        third = page(2000, 65537)
        third = np.concatenate((third[12345:], third[:12345]))
        fake = fake_module([first, second, third, page(2000, 0)])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        observed: list[int] = []

        def callback(ticks: POINTER(Mt5Tick), count: int, user_data: int) -> int:
            del user_data
            observed.extend(ticks[index].volume for index in range(count))
            return 0

        native_callback = self.tick_callback_type(callback)
        request = Mt5TicksRequest(b"EURUSD", 1000, 3000, 0)
        try:
            status = self.module.mt5bridge_copy_ticks_range(
                byref(request), 1024, native_callback, None
            )
            self.assertEqual(status, 0, self.last_error())
            self.assertEqual(len(observed), 65537)
            self.assertEqual(set(observed), set(range(65537)))
        finally:
            self.module.mt5bridge_shutdown()

    def test_copyticks_rows_before_requested_from_are_discarded(self) -> None:
        """Rows older than the requested range never cross the POD boundary."""
        values = page(1000, 3)
        values["time_msc"] = (1000, 2000, 2001)
        values["time"] = values["time_msc"] // 1000
        fake = fake_module([values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        request = Mt5TicksRequest(b"EURUSD", 2000, 3000, 0, 0)
        result = c_void_p()
        status = self.module.mt5bridge_query_ticks(byref(request), byref(result))
        try:
            self.assertEqual(status, 0, self.last_error())
            self.assertEqual(self.module.mt5bridge_tick_buffer_size(result), 2)
        finally:
            if result.value:
                self.module.mt5bridge_tick_buffer_free(result)
            self.module.mt5bridge_shutdown()

    def test_snapshot_delivery_and_staleness_are_rejected(self) -> None:
        """Unsupported coherent snapshots cannot be silently approximated."""
        fake = fake_module([])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        snapshot = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 8, 2, 0, (0, 0))
        stale = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 8, 0, 1, (0, 0))
        handle = Mt5SubscriptionHandle()
        try:
            self.assertNotEqual(self.module.mt5bridge_subscribe_ticks(byref(snapshot), byref(handle)), 0)
            self.assertIn("snapshot", self.last_error().lower())
            self.assertNotEqual(self.module.mt5bridge_subscribe_ticks(byref(stale), byref(handle)), 0)
            self.assertIn("stale_after", self.last_error())
        finally:
            self.module.mt5bridge_shutdown()

    def test_consumer_overflow_emits_explicit_gap(self) -> None:
        """A stalled consumer observes ring loss as a GAP, never silent success."""
        import time
        counter = [0]

        def next_tick(symbol: str, when: object) -> np.ndarray:
            del symbol
            counter[0] += 1
            values = page(int(when.timestamp() * 1000), 1)
            values["volume"] = counter[0]
            values["volume_real"] = counter[0]
            return values

        fake = fake_module([], history_sequence=[next_tick] * 20)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1, 1, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        gaps: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            if event.contents.type == MT5_SUBSCRIPTION_GAP:
                gaps.append(event.contents.gap_reason)
            return 0

        native_callback = self.event_callback_type(callback)
        time.sleep(0.35)
        try:
            self.module.mt5bridge_process_events(16, native_callback, None)
            self.assertIn(1, gaps)
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_grouped_subscription_preserves_source_indices(self) -> None:
        """Grouped raw delivery identifies members in request order."""
        import time

        def next_tick(symbol: str, when: object) -> np.ndarray:
            values = page(int(when.timestamp() * 1000), 1)
            values["volume"] = 1 if symbol == "EURUSD" else 2
            return values

        fake = fake_module([], history_sequence=[next_tick] * 20)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        sources = (Mt5TickSourceRequest * 2)(
            Mt5TickSourceRequest(b"EURUSD", 0, 0),
            Mt5TickSourceRequest(b"GBPUSD", 0, 0),
        )
        request = Mt5SubscriptionRequest(sources, 2, 10, 1, 8, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        indices: set[int] = set()

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            if event.contents.type == MT5_SUBSCRIPTION_TICK_BATCH and event.contents.count:
                indices.add(int(event.contents.source_index))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(40):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if indices == {0, 1}:
                break
        try:
            self.assertEqual(indices, {0, 1})
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_two_transient_failures_then_data_succeeds(self) -> None:
        """Bounded warm-up retries preserve the third successful response."""
        fake = fake_module(
            [
                (None, -4, "History timeout"),
                (None, 0, "History not ready"),
                page(2000, 2),
            ]
        )
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertEqual(diagnostics.attempts, 5)
        self.assertEqual(diagnostics.retries, 2)

    def test_empty_partial_full_history_is_confirmed(self) -> None:
        """A short success is probed again before the reader declares completion."""
        fake = fake_module(
            [
                page(2000, 0),
                (page(2000, 2), -10005, "IPC timeout"),
                page(2000, 2),
            ]
        )
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertGreaterEqual(diagnostics.reconnects, 1)

    def test_mql_history_timeout_4403_with_data_is_recovered(self) -> None:
        """MQL error 4403 with a non-empty page is not treated as complete."""
        fake = fake_module([
            (page(2000, 2), 4403, "History timeout"),
            page(2000, 2),
        ])
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertTrue(diagnostics.history_warmup_detected)

    def test_repeated_partial_4403_does_not_commit_incomplete_history(self) -> None:
        """Three partial 4403 pages are replayed from the original cursor."""
        partial = page(1200, 1)
        partial["time_msc"] = (1200,)
        complete = page(1100, 3)
        complete["time_msc"] = (1100, 1150, 1200)
        complete["time"] = complete["time_msc"] // 1000
        fake = fake_module([
            (partial, 4403, "History timeout"),
            (partial, 4403, "History timeout"),
            (partial, 4403, "History timeout"),
            complete,
        ])
        status, size, _, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 3)
        self.assertGreaterEqual(len(fake.request_starts), 4)
        self.assertEqual(fake.request_starts[:4], [1000, 1000, 1000, 1000])

    def test_empty_successes_can_be_followed_by_history_data(self) -> None:
        """Two empty success probes do not hide data arriving during warm-up."""
        fake = fake_module([page(2000, 0), page(2000, 0), page(2000, 2)])
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertGreaterEqual(diagnostics.attempts, 3)

    def test_short_page_progress_resets_completion_confirmation(self) -> None:
        """New ticks restart the EOF stability proof after empty probes."""
        d = page(2000, 1)
        de = page(2000, 2)
        de["time_msc"] = (2000, 2100)
        de["time"] = de["time_msc"] // 1000
        fake = fake_module([
            page(2000, 0),
            page(2000, 0),
            d,
            de,
            de,
            de,
        ])
        status, size, _, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertGreaterEqual(fake.calls, 6)

    def test_exact_full_page_followed_by_boundary_only_is_confirmed_complete(self) -> None:
        """A consumed short boundary page is a valid, probe-confirmed EOF."""
        first = page(2000, 65536)
        fake = fake_module([first, first, first, first])
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 65536)
        self.assertTrue(diagnostics.complete)
        self.assertGreaterEqual(fake.calls, 3)
        self.assertEqual(fake.request_counts[:2], [65536, 131072])

    def test_initialize_false_is_rejected(self) -> None:
        """A false MetaTrader initialize result is not accepted as success."""
        fake = fake_module([], initialize_result=False)
        sys.modules["MetaTrader5"] = fake
        status = self.module.mt5bridge_initialize(None)
        error = self.last_error()
        self.module.mt5bridge_shutdown()
        self.assertNotEqual(status, 0)
        self.assertIn("initialize", error.lower())

    def test_rates_recover_and_confirm_stable_result(self) -> None:
        """Rates use transient recovery and a bounded stable-result confirmation."""
        fake = fake_module(
            [],
            rate_sequence=[None, rate_page(2), rate_page(2)],
        )
        status, size, diagnostics, error = self.query_rates(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertEqual(diagnostics.retries, 2)
        self.assertTrue(diagnostics.history_warmup_detected)

    def test_rates_recovery_and_confirmation_have_separate_budgets(self) -> None:
        """A clean result after recovery still receives its confirmation probe."""
        fake = fake_module(
            [],
            rate_sequence=[None, None, rate_page(2), rate_page(2)],
        )
        status, size, diagnostics, error = self.query_rates(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertEqual(diagnostics.attempts, 4)
        self.assertEqual(diagnostics.retries, 3)
        self.assertTrue(diagnostics.history_warmup_detected)

    def test_repeated_page_fails_no_progress(self) -> None:
        """A repeated page cannot spin forever."""
        first = page(2000, 65536)
        older = page(1000, 65536)
        repeated = np.concatenate((older, first))
        fake = fake_module([first, repeated])
        status, _, diagnostics, error = self.query(fake)
        self.assertNotEqual(status, 0)
        self.assertLessEqual(fake.calls, 2)
        self.assertIn("non-progressing", error)
        self.assertEqual(diagnostics.status, 3)

    def test_short_stale_page_before_cursor_is_not_eof(self) -> None:
        """A short response older than the cursor is not valid completion."""
        first = page(2000, 65536)
        stale = page(1000, 3)
        stale["time_msc"] = (1000, 1500, 1900)
        stale["time"] = stale["time_msc"] // 1000
        fake = fake_module([first, stale])
        status, _, diagnostics, error = self.query(fake)
        self.assertNotEqual(status, 0)
        self.assertIn("before the active cursor", error)
        self.assertEqual(diagnostics.status, 3)

    def test_partial_page_continues_from_committed_cursor(self) -> None:
        """Partial data is retained and followed by an inclusive cursor retry."""
        partial = page(2000, 2)
        complete = page(2000, 3)
        complete[2]["time_msc"] = 2500
        fake = fake_module(
            [(partial, -10005, "IPC timeout"), (complete, 1, "Success")]
        )
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 3)
        self.assertEqual(diagnostics.attempts, 4)
        self.assertEqual(diagnostics.retries, 1)
        self.assertEqual(diagnostics.reconnects, 1)
        self.assertTrue(diagnostics.complete)

    def test_ipc_failure_reinitializes_before_retry(self) -> None:
        """An IPC failure reconnects once and resumes the idempotent read."""
        fake = fake_module(
            [(None, -10004, "No IPC connection"), page(2000, 0)]
        )
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 0)
        self.assertEqual(diagnostics.retries, 1)
        self.assertEqual(diagnostics.reconnects, 1)
        self.assertEqual(fake.initialize_calls, 2)
        self.assertEqual(fake.shutdown_calls, 2)

    def test_callback_can_reenter_bridge(self) -> None:
        """Native delivery runs without the runtime mutex held."""
        values = page(2000, 2)
        values["volume"] = (2**53 + 1, 42)
        values["volume_real"] = (1.25, 2.5)
        fake = fake_module([values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        request = Mt5TicksRequest(b"EURUSD", 1000, 3000, 0)
        callback_calls = 0
        observed_volumes: list[tuple[int, float]] = []

        def callback(ticks: POINTER(Mt5Tick), count: int, user_data: int) -> int:
            del user_data
            nonlocal callback_calls
            callback_calls += 1
            observed_volumes.extend(
                (ticks[index].volume, ticks[index].volume_real)
                for index in range(count)
            )
            response = c_void_p()
            status = self.module.mt5bridge_eval_json(
                b'{"method":"terminal_info"}', byref(response)
            )
            if response.value:
                self.module.mt5bridge_free(response)
            return status

        native_callback = self.tick_callback_type(callback)
        status = self.module.mt5bridge_copy_ticks_range(
            byref(request), 1, native_callback, None
        )
        error = self.last_error()
        self.module.mt5bridge_shutdown()
        self.assertEqual(status, 0, error)
        self.assertEqual(callback_calls, 2)
        self.assertEqual(observed_volumes, [(2**53 + 1, 1.25), (42, 2.5)])

    def test_legacy_order_method_is_disabled_without_side_effect(self) -> None:
        """The legacy JSON order method cannot reach MetaTrader order_send."""
        fake = fake_module([], order_error=TimeoutError("trade timeout"))
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        response = c_void_p()
        status = self.module.mt5bridge_eval_json(
            b'{"method":"open_market_buy","symbol":"EURUSD","volume":0.1}',
            byref(response),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(fake.order_calls, 0)
        self.assertIn("disabled", self.last_error())
        if response.value:
            self.module.mt5bridge_free(response)
        self.module.mt5bridge_shutdown()

    def test_legacy_order_method_does_not_probe_none_result(self) -> None:
        """The disabled path rejects before inspecting a trade result."""
        fake = fake_module([], order_none=True)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        response = c_void_p()
        status = self.module.mt5bridge_eval_json(
            b'{"method":"open_market_buy","symbol":"EURUSD","volume":0.1}',
            byref(response),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(fake.order_calls, 0)
        self.assertIn("disabled", self.last_error())
        if response.value:
            self.module.mt5bridge_free(response)
        self.module.mt5bridge_shutdown()

    def test_realtime_subscription_delivers_tied_timestamp_ticks(self) -> None:
        """Realtime overlap reconciliation preserves same-time tick multiplicity."""
        import time
        def make_values(when: object) -> np.ndarray:
            base = int(when.timestamp() * 1000)
            values = page(base, 3)
            values[0]["time_msc"] = values[1]["time_msc"] = base
            values[2]["time_msc"] = base
            values["time"] = values["time_msc"] // 1000
            return values

        fake = fake_module([], history_sequence=[make_values, make_values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1, 8, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == 0 and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].time_msc for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(30):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if len(observed) >= 3:
                break
        try:
            self.assertEqual(len(observed), 3)
            self.assertEqual(observed[0], observed[1])
            self.assertEqual(observed[2], observed[0])
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_realtime_subscription_does_not_emit_pre_subscription_ticks(self) -> None:
        """The initial overlap must not publish ticks before source creation."""
        import time
        base: list[int] = []

        def make_values(when: object) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000))
            values = page(base[0] - 100, 2)
            values["time_msc"] = (base[0] - 100, base[0])
            values["time"] = values["time_msc"] // 1000
            return values

        fake = fake_module([], history_sequence=[make_values, make_values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 8, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].time_msc for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(30):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if observed:
                break
        try:
            self.assertEqual(observed, [base[0]])
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_shared_source_keeps_smallest_delivery_batch_after_unsubscribe(self) -> None:
        """Removing a large subscriber must not widen the remaining batch contract."""
        import time
        def make_values(when: object) -> np.ndarray:
            values = page(int(when.timestamp() * 1000), 32)
            values["time_msc"] = int(when.timestamp() * 1000)
            values["time"] = values["time_msc"] // 1000
            return values

        fake = fake_module([], history_sequence=[make_values, make_values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        small = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1, 64, 0, 0, (0, 0))
        large = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1024, 64, 0, 0, (0, 0))
        small_handle = Mt5SubscriptionHandle()
        large_handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(small), byref(small_handle)), 0)
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(large), byref(large_handle)), 0)
        maximum_small = 0
        maximum_large = 0

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            nonlocal maximum_small, maximum_large
            if event.contents.type == MT5_SUBSCRIPTION_TICK_BATCH:
                if event.contents.handle.id == small_handle.id:
                    maximum_small = max(maximum_small, int(event.contents.count))
                else:
                    maximum_large = max(maximum_large, int(event.contents.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(40):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if maximum_small and maximum_large:
                break
        self.assertLessEqual(maximum_small, 1)
        self.assertGreater(maximum_large, 0)
        self.assertEqual(self.module.mt5bridge_unsubscribe(large_handle), 0)
        maximum_small = 0
        for _ in range(40):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if maximum_small:
                break
        self.assertGreater(maximum_small, 0)
        self.module.mt5bridge_unsubscribe(small_handle)
        self.module.mt5bridge_shutdown()
        self.assertLessEqual(maximum_small, 1)

    def test_committed_replay_redelivers_observed_but_unpublished_ticks(self) -> None:
        """A partial epoch must not suppress ticks from the committed replay."""
        import time
        base: list[int] = []
        expected: list[int] = []

        def make_page(count: int, when: object | None = None) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000) if when is not None else int(time.time() * 1000))
            values = page(base[0], count)
            values["time_msc"] = tuple(base[0] + index for index in range(count))
            values["time"] = values["time_msc"] // 1000
            if count == 3:
                expected[:] = [base[0], base[0] + 1, base[0] + 2]
            return values

        fake = fake_module(
            [],
            history_sequence=[
                lambda when: (make_page(1, when), -10005, "IPC timeout"),
                lambda when: make_page(2, when),
                lambda when: make_page(3, when),
                lambda when: make_page(3, when),
            ],
        )
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 32, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].time_msc for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(80):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, native_callback, None)
            if len(observed) >= 3:
                break
        try:
            self.assertEqual(observed, expected)
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_realtime_dense_pages_do_not_stall_and_expose_source_diagnostics(self) -> None:
        """Forward pagination remains lossless when the overlap is densely populated."""
        import time
        def make_values(when: object) -> np.ndarray:
            base = int(when.timestamp() * 1000)
            values = page(base, 32)
            values["time_msc"] = base
            values["time"] = values["time_msc"] // 1000
            return values

        fake = fake_module([], history_sequence=[make_values, make_values])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1, 64, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed = []
        def on_event(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            if event.contents.type == 0 and event.contents.count:
                ticks = ctypes.cast(event.contents.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[i].volume for i in range(event.contents.count))
            return 0
        callback = self.event_callback_type(on_event)
        for _ in range(30):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(128, callback, None)
            if len(observed) >= 32:
                break
        self.assertGreater(fake.calls, 3)
        self.assertEqual(len(observed), 32)
        self.assertEqual(len(set(observed)), 32)
        diagnostics = Mt5SubscriptionDiagnostics()
        self.assertEqual(
            self.module.mt5bridge_subscription_source_diagnostics(handle, 0, byref(diagnostics)), 0
        )
        self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
        self.module.mt5bridge_shutdown()

    def test_realtime_same_timestamp_reordering_between_pages_is_lossless(self) -> None:
        """Realtime pagination preserves a rotated same-timestamp boundary bucket."""
        import time
        base: list[int] = []

        def make_page(when: object, count: int, rotate: bool = False) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000))
            values = page(base[0], count)
            values["time_msc"] = base[0]
            values["time"] = values["time_msc"] // 1000
            return np.concatenate((values[-1:], values[:-1])) if rotate else values

        fake = fake_module(
            [],
            history_sequence=[
                lambda when: make_page(when, 1024),
                lambda when: make_page(when, 2049, True),
                lambda when: make_page(when, 2049, True),
            ],
        )
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1024, 8, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].volume for index in range(value.count))
                if len(observed) >= 2049:
                    return 1
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(80):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(128, native_callback, None)
            if len(observed) >= 2049:
                break
        try:
            self.assertEqual(len(observed), 2049, f"calls={fake.calls} observed={len(observed)}")
            self.assertEqual(set(observed), set(range(2049)))
            self.assertIn(2048, fake.request_counts)
            self.assertIn(3072, fake.request_counts)
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_realtime_overlap_reordering_does_not_report_rewrite(self) -> None:
        """Overlap pagination uses payload multiplicity across three reordered pages."""
        import time
        base: list[int] = []
        calls = [0]

        def make_page(when: object, count: int, rotate: int = 0) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000))
            values = page(base[0], count)
            values["time_msc"] = base[0]
            values["time"] = values["time_msc"] // 1000
            if rotate:
                values = np.concatenate((values[rotate:], values[:rotate]))
            return values

        def scripted_page(when: object) -> np.ndarray:
            calls[0] += 1
            return make_page(when, 2049, (calls[0] * 337) % 2049)

        fake = fake_module([], history_sequence=[scripted_page] * 64)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 1024, 16, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []
        gaps: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_GAP:
                gaps.append(value.gap_reason)
            elif value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].volume for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(160):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(128, native_callback, None)
            if fake.calls >= 12 and len(observed) >= 2049:
                break
        try:
            self.assertGreaterEqual(fake.calls, 12)
            self.assertEqual(len(observed), 2049)
            self.assertEqual(set(observed), set(range(2049)))
            self.assertNotIn(MT5_GAP_SOURCE_INCONSISTENCY, gaps)
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_realtime_tick_after_snapshot_horizon_does_not_repeat_tail(self) -> None:
        """A row arriving beyond captured now_msc cannot strand the accepted tail."""
        import time
        base: list[int] = []
        calls = [0]

        def scripted_page(symbol: str, when: object, count: int) -> np.ndarray:
            del symbol
            calls[0] += 1
            if not base:
                base.append(int(when.timestamp() * 1000))
            t0 = base[0]
            values = page(t0, 1 if calls[0] <= 2 else 3)
            if calls[0] <= 2:
                values["time_msc"] = (t0,)
                values["volume"] = (1,)
                values["volume_real"] = (0.1,)
            else:
                values["time_msc"] = (t0, t0 + 1, t0 + 60000)
                values["time"] = values["time_msc"] // 1000
                values["volume"] = (1, 2, 3)
                values["volume_real"] = (0.1, 0.2, 0.3)
            return values[:count]

        fake = fake_module([], history_sequence=[scripted_page] * 32)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 16, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].volume for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(80):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(64, native_callback, None)
            if calls[0] >= 4 and observed.count(2) >= 1:
                break
        try:
            self.assertGreaterEqual(calls[0], 4)
            self.assertEqual(observed.count(1), 1)
            self.assertEqual(observed.count(2), 1)
            self.assertEqual(observed.count(3), 0)
            for _ in range(10):
                time.sleep(0.02)
                self.module.mt5bridge_process_events(64, native_callback, None)
            self.assertEqual(observed.count(2), 1)
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()

    def test_realtime_late_insert_emits_recovery_gap(self) -> None:
        """A late MT5 history insertion is observable and delivered to the consumer."""
        import time
        base: list[int] = []

        def make_initial(when: object) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000))
            values = page(base[0], 2)
            values["time_msc"] = (base[0], base[0] + 2)
            values["time"] = values["time_msc"] // 1000
            return values

        fake = fake_module([], history_sequence=[make_initial, make_initial])
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 2, 16, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        gaps = []
        batches = []
        observed_ticks = []
        def on_event(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == 2:
                gaps.append(value.gap_reason)
            elif value.type == 0:
                batches.append(value.count)
                if value.count:
                    ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                    observed_ticks.extend((ticks[i].time_msc, ticks[i].volume)
                                          for i in range(value.count))
            return 0
        callback = self.event_callback_type(on_event)
        for _ in range(20):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, callback, None)
            if batches:
                break
        late = page(base[0], 3)
        late["time_msc"] = (base[0], base[0] + 1, base[0] + 2)
        late["time"] = late["time_msc"] // 1000
        fake.histories["EURUSD"] = late
        for _ in range(30):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, callback, None)
            if MT5_GAP_SOURCE_INCONSISTENCY in gaps:
                break
        self.assertIn(2, gaps)
        self.assertIn((base[0] + 1, 1), observed_ticks)
        self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
        self.module.mt5bridge_shutdown()

    def test_realtime_late_multiplicity_with_newer_tail_advances_cursor(self) -> None:
        """An A×2 to A×3 history change emits exactly one additional payload."""
        import time
        base: list[int] = []

        def make_values(when: object, count: int) -> np.ndarray:
            if not base:
                base.append(int(when.timestamp() * 1000))
            values = page(base[0], count)
            values["time_msc"] = base[0]
            values["time"] = values["time_msc"] // 1000
            values["volume"] = 7
            values["volume_real"] = 0.7
            return values

        fake = fake_module(
            [],
            history_sequence=[lambda when: make_values(when, 2), lambda when: make_values(when, 2)],
        )
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        source = Mt5TickSourceRequest(b"EURUSD", 0, 0)
        request = Mt5SubscriptionRequest(ctypes.pointer(source), 1, 10, 16, 16, 0, 0, (0, 0))
        handle = Mt5SubscriptionHandle()
        self.assertEqual(self.module.mt5bridge_subscribe_ticks(byref(request), byref(handle)), 0)
        observed: list[int] = []

        def callback(event: POINTER(Mt5SubscriptionEvent), user_data: int) -> int:
            del user_data
            value = event.contents
            if value.type == MT5_SUBSCRIPTION_TICK_BATCH and value.count:
                ticks = ctypes.cast(value.ticks, POINTER(Mt5Tick))
                observed.extend(ticks[index].volume for index in range(value.count))
            return 0

        native_callback = self.event_callback_type(callback)
        for _ in range(30):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, native_callback, None)
            if len(observed) >= 2:
                break
        for _ in range(30):
            if not fake.history_sequence:
                break
            time.sleep(0.01)
        # Drain events produced by the initial two-page publication before
        # changing the backing history. Otherwise a queued initial batch can
        # be delivered after the late update and look like a duplicate tail.
        for _ in range(10):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, native_callback, None)
        observed.clear()
        increased = page(base[0], 5)
        increased["time_msc"] = (base[0], base[0], base[0], base[0] + 1, base[0] + 1)
        increased["time"] = increased["time_msc"] // 1000
        increased["volume"] = (7, 7, 7, 8, 9)
        increased["volume_real"] = (0.7, 0.7, 0.7, 0.8, 0.9)
        fake.histories["EURUSD"] = increased
        for _ in range(40):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, native_callback, None)
            if len(observed) >= 3:
                break
        try:
            self.assertEqual(observed, [7, 8, 9], f"calls={fake.calls} observed={observed}")
            for _ in range(10):
                time.sleep(0.03)
                self.module.mt5bridge_process_events(64, native_callback, None)
            self.assertEqual(observed, [7, 8, 9])
        finally:
            self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
            self.module.mt5bridge_shutdown()


if __name__ == "__main__":
    unittest.main()

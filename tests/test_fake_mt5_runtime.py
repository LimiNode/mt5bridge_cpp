## \file test_fake_mt5_runtime.py
#  \brief Exercises recovery and pagination through a deterministic fake MT5 module.

"""Adversarial smoke tests for the CPython-backed bridge runtime.

Run after building the DLL with the same Python version as the test process:

    $env:MT5BRIDGE_DLL = "...\\mt5_bridge.dll"
    python -m unittest tests.test_fake_mt5_runtime
"""

from __future__ import annotations

import ctypes
import os
import sys
import types
import unittest
from ctypes import POINTER, Structure, byref, c_char_p, c_double, c_int, c_int32
from ctypes import c_int64, c_size_t, c_uint32, c_uint64, c_void_p

import numpy as np

MT5_GAP_SOURCE_INCONSISTENCY = 2
MT5_SUBSCRIPTION_TICK_BATCH = 0


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
    """Matches one ABI 7 physical source request."""

    _fields_ = [("symbol_utf8", c_char_p), ("flags", c_uint32), ("reserved", c_uint32)]


class Mt5SubscriptionRequest(Structure):
    """Matches the ABI 7 multi-source realtime subscription request."""

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
    """Matches the ABI 7 generation-qualified handle."""

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
) -> types.ModuleType:
    """Creates a fake MetaTrader5 module consuming a scripted sequence."""
    module = types.ModuleType("MetaTrader5")
    module.COPY_TICKS_ALL = 3
    module.TRADE_ACTION_DEAL = 1
    module.ORDER_TYPE_BUY = 0
    module.calls = 0
    module.initialize_calls = 0
    module.shutdown_calls = 0
    module.order_calls = 0
    module.last = (1, "Success")
    module.histories = histories or {}

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
        if symbol in module.histories:
            values = module.histories[symbol]
            start_msc = int(when.timestamp() * 1000)
            start = int(np.searchsorted(values["time_msc"], start_msc, side="left"))
            result = values[start : start + count]
            module.last = (1, "Success")
            return result
        result = sequence.pop(0) if sequence else page(2000, 0)
        if isinstance(result, tuple):
            result, code, message = result
            module.last = (code, message)
        elif result is None:
            module.last = (-4, "History timeout")
        else:
            module.last = (1, "Success")
        return result

    def order_send(request: dict[str, object]) -> dict[str, object] | None:
        if request.get("action") != module.TRADE_ACTION_DEAL or request.get("type") != module.ORDER_TYPE_BUY:
            raise ValueError("invalid market order request")
        module.order_calls += 1
        if order_error is not None:
            raise order_error
        if order_none:
            return None
        return {"retcode": 10009}

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
    module.terminal_info = terminal_info
    if rate_sequence is not None:
        module.copy_rates_range = copy_rates_range
    return module


class FakeMt5RuntimeTests(unittest.TestCase):
    """Verifies retry, page-boundary, and no-progress behavior without MT5."""

    @classmethod
    def setUpClass(cls) -> None:
        """Loads the DLL and installs the deterministic fake module."""
        if ctypes.sizeof(Mt5TicksRequest) != 32 or ctypes.sizeof(Mt5Tick) != 56:
            raise unittest.SkipTest("ctypes ABI layout does not match ABI version 7")
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
        if cls.module.mt5bridge_abi_version() != 7:
            raise unittest.SkipTest("test DLL does not expose ABI version 7")

    def tearDown(self) -> None:
        """Restores the module registry after each scenario."""
        sys.modules.pop("MetaTrader5", None)

    def last_error(self) -> str:
        """Returns the bridge diagnostic for an assertion message."""
        value = self.module.mt5bridge_last_error()
        return value.decode("utf-8", errors="replace") if value else ""

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

    def test_empty_successes_can_be_followed_by_history_data(self) -> None:
        """Two empty success probes do not hide data arriving during warm-up."""
        fake = fake_module([page(2000, 0), page(2000, 0), page(2000, 2)])
        status, size, diagnostics, error = self.query(fake)
        self.assertEqual(status, 0, error)
        self.assertEqual(size, 2)
        self.assertGreaterEqual(diagnostics.attempts, 3)

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
        fake = fake_module([page(2000, 65536), page(2000, 65536)])
        status, _, diagnostics, error = self.query(fake)
        self.assertNotEqual(status, 0)
        self.assertLessEqual(fake.calls, 2)
        self.assertIn("non-progressing", error)
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

    def test_order_timeout_path_has_one_call(self) -> None:
        """The side-effecting order operation is never retried by the bridge."""
        fake = fake_module([], order_error=TimeoutError("trade timeout"))
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        response = c_void_p()
        status = self.module.mt5bridge_eval_json(
            b'{"method":"open_market_buy","symbol":"EURUSD","volume":0.1}',
            byref(response),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(fake.order_calls, 1)
        if response.value:
            self.module.mt5bridge_free(response)
        self.module.mt5bridge_shutdown()

    def test_order_none_is_reported_as_failure(self) -> None:
        """MT5's None trade result must not become a successful JSON null."""
        fake = fake_module([], order_none=True)
        sys.modules["MetaTrader5"] = fake
        self.assertEqual(self.module.mt5bridge_initialize(None), 0)
        response = c_void_p()
        status = self.module.mt5bridge_eval_json(
            b'{"method":"open_market_buy","symbol":"EURUSD","volume":0.1}',
            byref(response),
        )
        self.assertNotEqual(status, 0)
        self.assertIn("returned None", self.last_error())
        if response.value:
            self.module.mt5bridge_free(response)
        self.module.mt5bridge_shutdown()

    def test_realtime_subscription_delivers_tied_timestamp_ticks(self) -> None:
        """Realtime overlap reconciliation preserves same-time tick multiplicity."""
        import time
        now = int(time.time() * 1000)
        values = page(now - 2, 3)
        values[0]["time_msc"] = values[1]["time_msc"] = now - 2
        values[2]["time_msc"] = now - 1
        values["time"] = values["time_msc"] // 1000
        fake = fake_module([], histories={"EURUSD": values})
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
        self.assertEqual(observed, [now - 2, now - 2, now - 1])
        self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
        self.module.mt5bridge_shutdown()

    def test_shared_source_keeps_smallest_delivery_batch_after_unsubscribe(self) -> None:
        """Removing a large subscriber must not widen the remaining batch contract."""
        import time
        now = int(time.time() * 1000)
        values = page(now - 1, 32)
        fake = fake_module([], histories={"EURUSD": values})
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
        self.assertEqual(self.module.mt5bridge_unsubscribe(large_handle), 0)
        maximum_small = 0
        for _ in range(40):
            time.sleep(0.02)
            self.module.mt5bridge_process_events(32, native_callback, None)
            if maximum_small:
                break
        self.module.mt5bridge_unsubscribe(small_handle)
        self.module.mt5bridge_shutdown()
        self.assertLessEqual(maximum_small, 1)

    def test_realtime_dense_pages_do_not_stall_and_expose_source_diagnostics(self) -> None:
        """Forward pagination remains lossless when the overlap is densely populated."""
        import time
        now = int(time.time() * 1000)
        values = page(now - 20, 32)
        values["time_msc"] = now - 20
        values["time"] = (now - 20) // 1000
        values[-3:]["time_msc"] = (now - 2, now - 1, now)
        values[-3:]["time"] = values[-3:]["time_msc"] // 1000
        fake = fake_module([], histories={"EURUSD": values})
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
            if len(observed) >= len(values):
                break
        self.assertGreater(fake.calls, 3)
        self.assertEqual(len(observed), len(values))
        self.assertEqual(len(set(observed)), len(values))
        diagnostics = Mt5SubscriptionDiagnostics()
        self.assertEqual(
            self.module.mt5bridge_subscription_source_diagnostics(handle, 0, byref(diagnostics)), 0
        )
        self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
        self.module.mt5bridge_shutdown()

    def test_realtime_late_insert_emits_recovery_gap(self) -> None:
        """A late MT5 history insertion is observable and delivered to the consumer."""
        import time
        now = int(time.time() * 1000)
        initial = page(now - 2, 2)
        initial["time_msc"] = (now - 2, now - 1)
        initial["time"] = initial["time_msc"] // 1000
        fake = fake_module([], histories={"EURUSD": initial})
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
        late = page(now - 3, 3)
        late["time_msc"] = (now - 3, now - 2, now - 1)
        late["time"] = late["time_msc"] // 1000
        fake.histories["EURUSD"] = late
        for _ in range(30):
            time.sleep(0.03)
            self.module.mt5bridge_process_events(64, callback, None)
            if MT5_GAP_SOURCE_INCONSISTENCY in gaps:
                break
        self.assertIn(2, gaps)
        self.assertIn((now - 3, 0), observed_ticks)
        self.assertEqual(self.module.mt5bridge_unsubscribe(handle), 0)
        self.module.mt5bridge_shutdown()


if __name__ == "__main__":
    unittest.main()

"""Smoke-check that the shipped ctypes adapter agrees with the DLL ABI."""

import ctypes
import unittest


class PythonAdapterTests(unittest.TestCase):
    """Imports the actual adapter and validates its version guard."""

    def test_adapter_abi_version(self) -> None:
        import mt5bridge_py

        self.assertEqual(mt5bridge_py._ABI_VERSION, 8)
        self.assertEqual(mt5bridge_py._lib.mt5bridge_abi_version(), 8)
        self.assertEqual(mt5bridge_py._TRADE_API_VERSION, 2)
        self.assertEqual(mt5bridge_py._lib.mt5bridge_trade_api_version(), 2)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5OrdersRequest), 32)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5PositionsRequest), 40)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5HistoryOrdersRequest), 48)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5HistoryDealsRequest), 56)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5OrderSnapshot), 472)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5PositionSnapshot), 440)
        self.assertEqual(ctypes.sizeof(mt5bridge_py.Mt5DealSnapshot), 440)


if __name__ == "__main__":
    unittest.main()

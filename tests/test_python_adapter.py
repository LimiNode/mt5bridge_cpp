"""Smoke-check that the shipped ctypes adapter agrees with the DLL ABI."""

import unittest


class PythonAdapterTests(unittest.TestCase):
    """Imports the actual adapter and validates its version guard."""

    def test_adapter_abi_version(self) -> None:
        import mt5bridge_py

        self.assertEqual(mt5bridge_py._ABI_VERSION, 7)
        self.assertEqual(mt5bridge_py._lib.mt5bridge_abi_version(), 7)


if __name__ == "__main__":
    unittest.main()

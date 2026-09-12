"""Unit checks for embeddable runtime staging safeguards."""

import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

from scripts.prepare_py_runtime import configure_embedded_path, extract_zip


class RuntimePreparationTests(unittest.TestCase):
    def test_pth_preserves_runtime_zip_name_and_adds_site_packages(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pth = root / "python312._pth"
            pth.write_text("python312.zip\n.\n", encoding="utf-8")
            configure_embedded_path(root)
            self.assertEqual(
                pth.read_text(encoding="utf-8"),
                "python312.zip\n.\nLib\\site-packages\nimport site\n",
            )

    def test_rejects_zip_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "unsafe.zip"
            with ZipFile(archive, "w") as zf:
                zf.writestr("../escape.txt", "blocked")
            with self.assertRaises(ValueError):
                extract_zip(archive, root / "out")


if __name__ == "__main__":
    unittest.main()

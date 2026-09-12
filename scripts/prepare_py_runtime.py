#!/usr/bin/env python3
## \file prepare_py_runtime.py
#  \brief Downloads and assembles an embeddable Python runtime for mt5bridge.

"""Utility script to prepare an embeddable Python runtime.

It downloads the Windows embeddable distribution of Python, unzips it to the
specified directory and extracts given wheel files into ``Lib/site-packages``.
Finally, the project's ``mt5bridge_py.py`` helper module is copied into the
site-packages directory so the runtime can import it.

Example
-------

.. code-block:: bash

    python scripts/prepare_py_runtime.py \
        --python-url https://www.python.org/ftp/python/3.11.5/python-3.11.5-embed-amd64.zip \
        --wheels https://files.pythonhosted.org/packages/.../numpy.whl \
        --output py_runtime
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path
import os
from zipfile import ZipFile


## \brief Number of bytes transferred per download read.
CHUNK_SIZE = 1024 * 1024  # 1 MiB


## \brief Downloads a URL to a local file while reporting basic progress.
#  \param url Source URL.
#  \param dest Destination file path.
def download(url: str, dest: Path) -> None:
    """Download *url* to *dest* displaying basic progress."""
    with urllib.request.urlopen(url) as response, dest.open("wb") as fh:
        total = int(response.getheader("Content-Length", 0))
        downloaded = 0
        while True:
            chunk = response.read(CHUNK_SIZE)
            if not chunk:
                break
            fh.write(chunk)
            downloaded += len(chunk)
            if total:
                percent = downloaded * 100 // total
                print(f"Downloaded {percent}%", end="\r", flush=True)
    print()


## \brief Extracts a Python distribution archive.
#  \param archive Source ZIP file.
#  \param dest Destination directory.
def extract_zip(archive: Path, dest: Path) -> None:
    """Unzip *archive* into *dest*."""
    with ZipFile(archive) as zf:
        root = dest.resolve()
        for info in zf.infolist():
            target = (dest / info.filename).resolve()
            if os.path.commonpath((str(root), str(target))) != str(root):
                raise ValueError(f"unsafe ZIP path: {info.filename}")
        zf.extractall(dest)


## \brief Extracts a wheel into the prepared site-packages directory.
#  \param wheel Source wheel file.
#  \param site_packages Destination site-packages directory.
def extract_wheel(wheel: Path, site_packages: Path) -> None:
    """Extract *wheel* into *site_packages*."""
    with ZipFile(wheel) as zf:
        root = site_packages.resolve()
        for info in zf.infolist():
            target = (site_packages / info.filename).resolve()
            if os.path.commonpath((str(root), str(target))) != str(root):
                raise ValueError(f"unsafe wheel path: {info.filename}")
        zf.extractall(site_packages)


## \brief Enables imports from the embedded runtime's site-packages directory.
#  \param runtime_dir Extracted embeddable Python directory.
def configure_embedded_path(runtime_dir: Path) -> None:
    """Configure the embeddable interpreter's isolated ``._pth`` file."""
    pth_files = list(runtime_dir.glob("python*._pth"))
    if not pth_files:
        return
    pth = pth_files[0]
    pth.write_text("python311.zip\n.\nLib\\site-packages\nimport site\n", encoding="utf-8")


## \brief Parses command-line options and assembles the runtime directory.
#  \param argv Optional argument list; None uses process arguments.
def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-url", required=True,
                        help="URL to embeddable Python zip distribution")
    parser.add_argument("--wheels", nargs="*", default=[],
                        help="Wheel file URLs to install into the runtime")
    parser.add_argument("--output", default="py_runtime",
                        help="Destination directory for the extracted runtime")
    args = parser.parse_args(argv)

    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    python_zip = output_dir / "python_embed.zip"
    print(f"Downloading Python runtime from {args.python_url}...")
    download(args.python_url, python_zip)

    print("Extracting Python runtime...")
    extract_zip(python_zip, output_dir)
    configure_embedded_path(output_dir)

    site_packages = output_dir / "Lib" / "site-packages"
    site_packages.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        for url in args.wheels:
            wheel_name = url.rsplit("/", 1)[-1]
            wheel_path = tmpdir_path / wheel_name
            print(f"Downloading wheel {wheel_name}...")
            download(url, wheel_path)
            print(f"Extracting {wheel_name}...")
            extract_wheel(wheel_path, site_packages)

    repo_root = Path(__file__).resolve().parents[1]
    src_bridge = repo_root / "python" / "mt5bridge_py.py"
    if not src_bridge.exists():
        print(f"Could not find {src_bridge}", file=sys.stderr)
        sys.exit(1)
    shutil.copy2(src_bridge, site_packages / "mt5bridge_py.py")
    print("Copied mt5bridge_py.py into site-packages")


if __name__ == "__main__":  # pragma: no cover
    main()

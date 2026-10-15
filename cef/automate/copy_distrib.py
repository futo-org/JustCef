#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import tempfile


PLATFORMS = {
    ("linux", "x64"): "linux64",
    ("linux", "arm64"): "linuxarm64",
    ("windows", "x64"): "windows64",
    ("windows", "arm64"): "windowsarm64",
    ("macos", "x64"): "macosx64",
    ("macos", "arm64"): "macosarm64",
}


def copy_distribution(download_dir: Path, output_dir: Path, platform: str, arch: str) -> Path:
    download_dir = download_dir.resolve()
    output_dir = output_dir.resolve()
    destination_dir = (output_dir / f"{platform}-{arch}").resolve()
    for checkout in ((download_dir / "cef").resolve(), (download_dir / "chromium/src").resolve()):
        if any(path == checkout or checkout in path.parents for path in (output_dir, destination_dir)):
            raise ValueError(f"Output directory must be outside the cleaned checkouts: {output_dir}")

    source_dir = download_dir / "chromium/src/cef/binary_distrib"
    pattern = f"cef_binary_*_{PLATFORMS[platform, arch]}_minimal.tar.bz2"
    archives = sorted(path for path in source_dir.glob(pattern) if path.is_file())
    if len(archives) != 1:
        raise ValueError(f"Expected one {pattern} archive in {source_dir}; found {len(archives)}")
    source = archives[0]
    if source.stat().st_size == 0:
        raise ValueError(f"Distribution archive is empty: {source}")

    destination_dir.mkdir(parents=True, exist_ok=True)
    destination = destination_dir / source.name
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{source.name}.", suffix=".tmp", dir=destination_dir)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copy2(source, temporary)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--download-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("linux", "windows", "macos"))
    parser.add_argument("--arch", required=True, choices=("x64", "arm64"))
    args = parser.parse_args()
    destination = copy_distribution(args.download_dir, args.output_dir, args.platform, args.arch)
    print(f"==> Saved CEF distribution: {destination}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"Failed to preserve CEF distribution: {error}")

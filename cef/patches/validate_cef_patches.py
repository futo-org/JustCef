#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"Command failed: {' '.join(args)}\n{result.stdout}{result.stderr}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cef-dir", required=True, type=Path)
    parser.add_argument("--ref", help="Override the checked-in CEF revision for compatibility checks")
    args = parser.parse_args()
    repository = args.cef_dir.resolve()
    version = (ROOT / "cef.version").read_text().strip()
    match = re.fullmatch(r"\d+\.\d+\.\d+\+g([0-9a-f]+)\+chromium-[\d.]+", version)
    if not match:
        parser.error(f"Invalid cef.version: {version}")
    revision = args.ref or match[1]
    commit = run("git", "rev-parse", "--verify", f"{revision}^{{commit}}", cwd=repository).strip()
    patches = sorted(path for path in (ROOT / "patches/cef").rglob("*.patch") if path.is_file())
    if not patches:
        parser.error("No CEF patches found")

    with tempfile.TemporaryDirectory(prefix="justcef-patch-check-") as directory:
        checkout = Path(directory) / "cef"
        run("git", "worktree", "add", "--quiet", "--detach", str(checkout), commit, cwd=repository)
        try:
            for patch in patches:
                run("git", "apply", "--check", str(patch), cwd=checkout)
                run("git", "apply", "--whitespace=nowarn", str(patch), cwd=checkout)
            before = run("git", "diff", "--binary", "HEAD", cwd=checkout)
            untracked = run("git", "ls-files", "--others", "--exclude-standard", cwd=checkout)
            contents = {name: (checkout / name).read_bytes() for name in untracked.splitlines()}

            run(sys.executable, str(ROOT / "patches/apply_cef_patches.py"), "--cef-dir", str(checkout))
            if before != run("git", "diff", "--binary", "HEAD", cwd=checkout):
                raise RuntimeError("Repeated application changed tracked files")
            if untracked != run("git", "ls-files", "--others", "--exclude-standard", cwd=checkout):
                raise RuntimeError("Repeated application changed the untracked file list")
            if any((checkout / name).read_bytes() != value for name, value in contents.items()):
                raise RuntimeError("Repeated application changed added files")

            for patch in reversed(patches):
                run("git", "apply", "--reverse", "--check", str(patch), cwd=checkout)
                run("git", "apply", "--reverse", "--whitespace=nowarn", str(patch), cwd=checkout)
            if run("git", "status", "--porcelain", cwd=checkout).strip():
                raise RuntimeError("Reversing the stack did not restore the clean checkout")
            print(f"PASS: {len(patches)} CEF patches, full context, repeated application and reversal at {commit}")
        finally:
            run("git", "worktree", "remove", "--force", str(checkout), cwd=repository)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as error:
        raise SystemExit(str(error))

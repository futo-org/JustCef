#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run_git_apply(cef_dir: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "apply", *args],
        cwd=cef_dir,
        capture_output=True,
        text=True,
        check=False,
    )


def command_output(result: subprocess.CompletedProcess[str]) -> str:
    return (result.stdout + result.stderr).strip()


def apply_patch_file(cef_dir: Path, patch_file: Path) -> None:
    reverse_check = run_git_apply(cef_dir, "--reverse", "--check", str(patch_file))
    if reverse_check.returncode == 0:
        print(f"Patch already applied: {patch_file.name}")
        return

    forward_check = run_git_apply(cef_dir, "--check", str(patch_file))
    if forward_check.returncode == 0:
        apply_result = run_git_apply(
            cef_dir, "--verbose", "--whitespace=nowarn", str(patch_file)
        )
        if apply_result.returncode != 0:
            raise RuntimeError(
                f"Failed to apply patch {patch_file}:\n{command_output(apply_result)}"
            )
        print(f"Applied patch: {patch_file.name}")
        return

    merge_result = run_git_apply(
        cef_dir, "--3way", "--verbose", "--whitespace=nowarn", str(patch_file)
    )
    if merge_result.returncode == 0:
        print(f"Applied patch with 3-way merge: {patch_file.name}")
        return

    raise RuntimeError(
        f"Failed to apply patch {patch_file}.\n"
        f"--check output:\n{command_output(forward_check)}\n\n"
        f"--3way output:\n{command_output(merge_result)}"
    )


def patch_files(patch_dir: Path) -> list[Path]:
    return sorted(path for path in patch_dir.rglob("*.patch") if path.is_file())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cef-dir", required=True)
    parser.add_argument("--cef-branch")
    parser.add_argument("--patch-dir")
    args = parser.parse_args()

    cef_dir = Path(args.cef_dir).resolve()
    if not (cef_dir / ".git").exists():
        raise SystemExit(f"CEF checkout is not a git repository: {cef_dir}")

    patch_dir = (
        Path(args.patch_dir).resolve()
        if args.patch_dir
        else Path(__file__).resolve().parent / "cef"
    )
    if not patch_dir.is_dir():
        raise SystemExit(f"Patch directory does not exist: {patch_dir}")

    patches = patch_files(patch_dir)
    if not patches:
        raise SystemExit(f"No patch files found in {patch_dir}")

    for patch_file in patches:
        apply_patch_file(cef_dir, patch_file)

    print(f"Applied custom CEF patches from {patch_dir} into {cef_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

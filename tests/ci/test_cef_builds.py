import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("copy_distrib", ROOT / "cef/automate/copy_distrib.py")
COPY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COPY)


class DistributionCopyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cef copies ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.download = self.root / "checkout"
        self.source = self.download / "chromium/src/cef/binary_distrib"
        self.source.mkdir(parents=True)
        self.output = self.root / "artifacts"

    def archive(self, platform="linux64", version="test", content=b"archive"):
        path = self.source / f"cef_binary_{version}_{platform}_minimal.tar.bz2"
        path.write_bytes(content)
        return path

    def test_all_platforms_and_architectures_survive_checkout_cleanup(self):
        for platform, arch, suffix in (
            ("linux", "x64", "linux64"), ("linux", "arm64", "linuxarm64"),
            ("windows", "x64", "windows64"), ("windows", "arm64", "windowsarm64"),
            ("macos", "x64", "macosx64"), ("macos", "arm64", "macosarm64"),
        ):
            with self.subTest(platform=platform, arch=arch):
                source = self.archive(suffix)
                saved = COPY.copy_distribution(self.download, self.output, platform, arch)
                self.assertEqual(saved, self.output / f"{platform}-{arch}" / source.name)
                source.unlink()
                self.assertEqual(saved.read_bytes(), b"archive")

    def test_missing_ambiguous_and_empty_archives_fail(self):
        with self.assertRaises(ValueError):
            COPY.copy_distribution(self.download, self.output, "linux", "x64")
        self.archive(content=b"")
        with self.assertRaises(ValueError):
            COPY.copy_distribution(self.download, self.output, "linux", "x64")
        self.archive(version="other")
        with self.assertRaises(ValueError):
            COPY.copy_distribution(self.download, self.output, "linux", "x64")
        self.assertFalse(self.output.exists())

    def test_failed_copy_keeps_previous_archive_and_removes_partial_file(self):
        source = self.archive(content=b"previous")
        saved = COPY.copy_distribution(self.download, self.output, "linux", "x64")
        source.write_bytes(b"replacement")

        def fail_copy(source, destination):
            Path(destination).write_bytes(b"partial")
            raise OSError("disk full")

        with mock.patch.object(COPY.shutil, "copy2", side_effect=fail_copy):
            with self.assertRaises(OSError):
                COPY.copy_distribution(self.download, self.output, "linux", "x64")
        self.assertEqual(saved.read_bytes(), b"previous")
        self.assertEqual(list(saved.parent.iterdir()), [saved])

    def test_other_versions_and_architectures_are_preserved(self):
        source = self.archive()
        saved = COPY.copy_distribution(self.download, self.output, "linux", "x64")
        source.unlink()
        replacement = self.archive(version="new")
        other = self.archive(platform="linuxarm64")
        COPY.copy_distribution(self.download, self.output, "linux", "x64")
        COPY.copy_distribution(self.download, self.output, "linux", "arm64")
        self.assertTrue(saved.exists())
        self.assertTrue((saved.parent / replacement.name).exists())
        self.assertTrue((self.output / "linux-arm64" / other.name).exists())

    def test_output_inside_cleaned_checkout_is_rejected(self):
        self.archive()
        for output in (self.download / "cef", self.download / "chromium/src/archives", self.source):
            with self.subTest(output=output), self.assertRaises(ValueError):
                COPY.copy_distribution(self.download, output, "linux", "x64")

    @unittest.skipIf(os.name == "nt", "Symlinks require Windows privileges")
    def test_architecture_symlink_into_checkout_is_rejected(self):
        self.archive()
        self.output.mkdir()
        (self.output / "linux-x64").symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(ValueError):
            COPY.copy_distribution(self.download, self.output, "linux", "x64")

    @unittest.skipIf(os.name == "nt", "Symlinks require Windows privileges")
    def test_symlinked_checkout_output_is_rejected(self):
        self.archive()
        checkout = self.download / "chromium/src"
        relocated = self.root / "relocated source"
        checkout.rename(relocated)
        checkout.symlink_to(relocated, target_is_directory=True)
        with self.assertRaises(ValueError):
            COPY.copy_distribution(self.download, relocated / "archives", "linux", "x64")


FAKE_AUTOMATE = r'''
import argparse
import os
from pathlib import Path
import shutil
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--download-dir", type=Path)
args, rest = parser.parse_known_args()
assert "--force-distrib" in rest
arch = "arm64" if "--arm64-build" in rest else "x64"
with open(os.environ["TEST_CALLS"], "a") as log:
    log.write(arch + "\n")
source = args.download_dir / "chromium/src/cef"
if source.exists():
    shutil.rmtree(source)
source = source / "binary_distrib"
source.mkdir(parents=True)
platform = os.environ["TEST_PLATFORM"]
suffix = {("linux", "x64"): "linux64", ("linux", "arm64"): "linuxarm64",
          ("macos", "x64"): "macosx64", ("macos", "arm64"): "macosarm64"}[platform, arch]
if os.environ.get("TEST_MISSING_ARCH") != arch:
    (source / ("cef_binary_fixture_" + suffix + "_minimal.tar.bz2")).write_text(arch)
if os.environ.get("TEST_FAIL_ARCH") == arch:
    sys.exit(23)
'''


@unittest.skipIf(os.name == "nt", "Requires a POSIX shell")
class PosixBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cef builds ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("cef.version", "cef.branch", "automate/copy_distrib.py"):
            target = self.root / "cef" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / "cef" / name, target)
        for platform in ("aws-linux-x64", "macos"):
            for name in ("build.sh", "build-x64.sh", "build-arm64.sh"):
                target = self.root / "cef" / platform / "scripts" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / "cef" / platform / "scripts" / name, target)
        (self.root / "cef/automate/automate-git.py").write_text(FAKE_AUTOMATE)
        self.download = self.root / "checkout"
        for name in ("cef/.git", "chromium/src/.git"):
            (self.download / name).mkdir(parents=True)
        (self.root / "depot/.git").mkdir(parents=True)
        self.calls = self.root / "calls"
        self.env = dict(os.environ, DOWNLOAD_DIR=str(self.download), DEPOT_TOOLS_DIR=str(self.root / "depot"),
                        PYTHON_BIN=sys.executable, TEST_CALLS=str(self.calls))
        self.env.pop("CEF_OUTPUT_DIR", None)

    def build(self, platform, *args, script="build.sh"):
        directory = "aws-linux-x64" if platform == "linux" else "macos"
        self.env["TEST_PLATFORM"] = platform
        return subprocess.run(["sh", str(self.root / "cef" / directory / "scripts" / script), *args],
                              env=self.env, cwd=self.root, capture_output=True, text=True)

    def saved(self, platform, arch):
        output = Path(self.env.get("CEF_OUTPUT_DIR", str(self.root / "cef/artifacts")))
        return list((output / f"{platform}-{arch}").glob("*.tar.bz2"))

    def test_combined_builds_preserve_both_archives(self):
        for platform in ("linux", "macos"):
            for arguments in ((), ("all",)):
                with self.subTest(platform=platform, arguments=arguments):
                    self.calls.unlink(missing_ok=True)
                    result = self.build(platform, *arguments)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.calls.read_text().splitlines(), ["x64", "arm64"])
                    self.assertEqual(self.saved(platform, "x64")[0].read_text(), "x64")
                    self.assertEqual(self.saved(platform, "arm64")[0].read_text(), "arm64")
                    archives = (self.download / "chromium/src/cef/binary_distrib").glob("*.tar.bz2")
                    self.assertEqual([archive.read_text() for archive in archives], ["arm64"])

    def test_build_failure_stops_immediately_without_copying_failed_archive(self):
        for platform in ("linux", "macos"):
            self.env["TEST_FAIL_ARCH"] = "x64"
            self.calls.unlink(missing_ok=True)
            result = self.build(platform, "all")
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
            self.assertEqual(self.calls.read_text().splitlines(), ["x64"])
            self.assertFalse(self.saved(platform, "x64"))
            self.assertFalse(self.saved(platform, "arm64"))

    def test_second_build_failure_preserves_first_archive(self):
        for platform in ("linux", "macos"):
            self.env["TEST_FAIL_ARCH"] = "arm64"
            result = self.build(platform, "all")
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
            self.assertEqual(self.saved(platform, "x64")[0].read_text(), "x64")
            self.assertFalse(self.saved(platform, "arm64"))

    def test_missing_distribution_stops_before_next_build(self):
        for platform in ("linux", "macos"):
            self.env["TEST_MISSING_ARCH"] = "x64"
            self.calls.unlink(missing_ok=True)
            result = self.build(platform, "all")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.calls.read_text().splitlines(), ["x64"])

    def test_copy_failure_stops_before_next_build(self):
        for platform in ("linux", "macos"):
            output = self.root / "cef/artifacts"
            output.mkdir(exist_ok=True)
            (output / f"{platform}-x64").write_text("blocked")
            self.calls.unlink(missing_ok=True)
            result = self.build(platform, "all")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.calls.read_text().splitlines(), ["x64"])

    def test_separate_wrappers_and_custom_output(self):
        self.env["CEF_OUTPUT_DIR"] = str(self.root / "custom output")
        for platform in ("linux", "macos"):
            for arch in ("x64", "arm64"):
                with self.subTest(platform=platform, arch=arch):
                    result = self.build(platform, script=f"build-{arch}.sh")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.saved(platform, arch)[0].read_text(), arch)


FAKE_WINDOWS_PYTHON = r'''@echo off
setlocal EnableDelayedExpansion
set "ARGS=%*"
set "ARCH=x64"
if not "!ARGS:--arm64-build=!"=="!ARGS!" set "ARCH=arm64"
if "%~nx1"=="copy_distrib.py" goto preserve
set "SUFFIX=windows64"
if "!ARCH!"=="arm64" set "SUFFIX=windowsarm64"
echo build !ARCH!>>"%TEST_LOG%"
if exist "%DOWNLOAD_DIR%\chromium\src\cef" rmdir /s /q "%DOWNLOAD_DIR%\chromium\src\cef"
mkdir "%DOWNLOAD_DIR%\chromium\src\cef\binary_distrib"
echo !ARCH!>"%DOWNLOAD_DIR%\chromium\src\cef\binary_distrib\cef_binary_fixture_!SUFFIX!_minimal.tar.bz2"
if "%TEST_FAIL_BUILD%"=="!ARCH!" exit /b 23
exit /b 0
:preserve
set "ARCH=x64"
if not "!ARGS:arm64=!"=="!ARGS!" set "ARCH=arm64"
set "SUFFIX=windows64"
if "!ARCH!"=="arm64" set "SUFFIX=windowsarm64"
echo copy !ARCH!>>"%TEST_LOG%"
if "%TEST_FAIL_COPY%"=="!ARCH!" exit /b 29
if not exist "%CEF_OUTPUT_DIR%\windows-!ARCH!" mkdir "%CEF_OUTPUT_DIR%\windows-!ARCH!"
copy /y "%DOWNLOAD_DIR%\chromium\src\cef\binary_distrib\cef_binary_fixture_!SUFFIX!_minimal.tar.bz2" "%CEF_OUTPUT_DIR%\windows-!ARCH!\" >nul
exit /b %ERRORLEVEL%
'''


@unittest.skipUnless(os.name == "nt" or (shutil.which("wine") and os.environ.get("CEF_TEST_WINEPREFIX")),
                     "Requires Windows cmd or Wine with CEF_TEST_WINEPREFIX")
class WindowsBuildTests(unittest.TestCase):
    def test_combined_separate_and_failure_paths(self):
        with tempfile.TemporaryDirectory(prefix="cef win builds ") as directory:
            root = Path(directory)
            cef = root / "repo/cef"
            for name in ("build.bat", "build-x64.bat", "build-arm64.bat"):
                target = cef / "aws-windows-x64/scripts" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / "cef/aws-windows-x64/scripts" / name, target)
            for name in ("cef.branch", "cef.version"):
                shutil.copy2(ROOT / "cef" / name, cef / name)
            binaries = root / "bin"
            binaries.mkdir()

            def batch(path, text):
                path.write_bytes(text.replace("\n", "\r\n").encode())

            def windows_path(path):
                return str(path) if os.name == "nt" else "Z:" + str(path).replace("/", "\\")

            batch(binaries / "powershell.bat", "@echo off\nexit /b 0\n")
            batch(binaries / "python.bat", FAKE_WINDOWS_PYTHON)
            env = dict(os.environ)
            if os.name != "nt":
                env.update(WINEDEBUG="-all", WINEPREFIX=env["CEF_TEST_WINEPREFIX"])
            for name, arguments, fail_build, fail_copy, expected, script in (
                ("combined", "all", "", "", 0, "build.bat"),
                ("default", "", "", "", 0, "build.bat"),
                ("first-failure", "all", "x64", "", 23, "build.bat"),
                ("second-failure", "all", "arm64", "", 23, "build.bat"),
                ("copy-failure", "all", "", "x64", 29, "build.bat"),
                ("second-copy-failure", "all", "", "arm64", 29, "build.bat"),
                ("x64-only", "", "", "", 0, "build-x64.bat"),
                ("arm64-only", "", "", "", 0, "build-arm64.bat"),
            ):
                with self.subTest(scenario=name):
                    case = root / name
                    case.mkdir()
                    output = case / "output archives"
                    log = case / "calls"
                    driver = case / "run.bat"
                    output_setting = "" if name == "default" else windows_path(output)
                    batch(driver, f'''@echo off
set "PATH={windows_path(binaries)};%PATH%"
set "DOWNLOAD_DIR={windows_path(case / 'checkout')}"
set "DEPOT_TOOLS_DIR={windows_path(case / 'depot')}"
set "CEF_OUTPUT_DIR={output_setting}"
set "TEST_LOG={windows_path(log)}"
set "TEST_FAIL_BUILD={fail_build}"
set "TEST_FAIL_COPY={fail_copy}"
call "{windows_path(cef / 'aws-windows-x64/scripts' / script)}" {arguments}
exit /b %ERRORLEVEL%
''')
                    command = ["cmd", "/d", "/c", windows_path(driver)]
                    if os.name != "nt":
                        command.insert(0, "wine")
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=45)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    calls = log.read_text().splitlines()
                    expected_calls = ["build x64", "copy x64", "build arm64", "copy arm64"]
                    if fail_build == "x64":
                        expected_calls = expected_calls[:1]
                    elif fail_copy == "x64" or script == "build-x64.bat":
                        expected_calls = expected_calls[:2]
                    elif fail_build == "arm64":
                        expected_calls = expected_calls[:3]
                    elif script == "build-arm64.bat":
                        expected_calls = expected_calls[2:]
                    self.assertEqual(calls, expected_calls)
                    if name == "default":
                        output = cef / "artifacts"
                    saved_x64 = list((output / "windows-x64").glob("*.tar.bz2"))
                    saved_arm64 = list((output / "windows-arm64").glob("*.tar.bz2"))
                    if fail_build != "x64" and fail_copy != "x64" and script != "build-arm64.bat":
                        self.assertEqual(saved_x64[0].read_text().strip(), "x64")
                    else:
                        self.assertFalse(saved_x64)
                    if not fail_build and not fail_copy and script != "build-x64.bat":
                        self.assertEqual(saved_arm64[0].read_text().strip(), "arm64")
                    else:
                        self.assertFalse(saved_arm64)


if __name__ == "__main__":
    unittest.main()

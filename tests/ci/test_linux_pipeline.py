import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class LinuxPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for relative in ("native/build-linux.sh", "native/ci/publish-linux.sh"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        (self.root / "native/src").mkdir()
        (self.root / "native/src/CMakeLists.txt").write_text("add_compile_definitions(JUSTCEF_NATIVE_VERSION=9)\n")
        binaries = self.root / "bin"
        binaries.mkdir()
        docker = binaries / "docker"
        docker.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >> "$DOCKER_CALLS"\nif [[ "$1 $2" == "buildx build" ]]; then echo compiler-output; exit "${BUILD_EXIT:-0}"; fi\n')
        docker.chmod(0o755)
        self.calls = self.root / "docker-calls"
        self.env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ["PATH"],
                        DOCKER_CALLS=str(self.calls), CI_COMMIT_SHA="a" * 40,
                        CI_COMMIT_TAG="9", BUILDX_BUILDER="test", BUILD_JOBS="2",
                        CF_R2_ACCOUNT_ID="account", CF_R2_BUCKET="bucket",
                        CF_R2_ACCESS_KEY_ID="test-key", CF_R2_SECRET_ACCESS_KEY="test-secret")

    def run_script(self, script, *args):
        return subprocess.run(["bash", str(self.root / script), *args], env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def prepare_artifacts(self):
        for architecture in ("x64", "arm64"):
            output = self.root / "build" / ("linux-" + architecture)
            output.mkdir(parents=True)
            (output / "build-info.txt").write_text("revision=" + "a" * 40 + "\nversion=9\narchitecture=" + architecture + "\n")
            archive = "JustCefNative-linux-" + architecture + ".zip"
            (output / archive).write_bytes(b"test artifact")
            (output / (archive + ".sha256")).write_text(hashlib.sha256(b"test artifact").hexdigest() + "  " + archive + "\n")

    def test_build_architectures(self):
        for architecture, platform in (("x64", "linux/amd64"), ("arm64", "linux/arm64")):
            result = self.run_script("native/build-linux.sh", architecture)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("--platform " + platform, self.calls.read_text())
            self.assertIn("RELEASE_VERSION=9", self.calls.read_text())

    def test_build_failure_preserves_log_and_exit_code(self):
        self.env["BUILD_EXIT"] = "23"
        result = self.run_script("native/build-linux.sh", "x64")
        self.assertEqual(result.returncode, 23, result.stdout)
        self.assertIn("compiler-output", (self.root / "build/linux-x64/build.log").read_text())

    def test_wrong_tag_never_builds(self):
        self.env["CI_COMMIT_TAG"] = "10"
        result = self.run_script("native/build-linux.sh", "x64")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("buildx build", self.calls.read_text())

    def test_invalid_architecture_never_calls_docker(self):
        result = self.run_script("native/build-linux.sh", "invalid")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.calls.exists())

    def test_publish_validates_both_architectures_before_upload(self):
        self.prepare_artifacts()
        (self.root / "build/linux-arm64/JustCefNative-linux-arm64.zip").write_bytes(b"corrupt")
        result = self.run_script("native/ci/publish-linux.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_publish_rejects_wrong_commit(self):
        self.prepare_artifacts()
        self.env["CI_COMMIT_SHA"] = "b" * 40
        result = self.run_script("native/ci/publish-linux.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.calls.exists())

    def test_publish_uses_versioned_paths_without_secret_arguments(self):
        self.prepare_artifacts()
        result = self.run_script("native/ci/publish-linux.sh")
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.calls.read_text()
        self.assertEqual(len(calls.splitlines()), 4)
        self.assertIn("s3://bucket/justcef/9/JustCefNative-linux-arm64.zip", calls)
        self.assertNotIn("test-secret", calls)
        self.assertNotIn("test-key", calls)


if __name__ == "__main__":
    unittest.main()

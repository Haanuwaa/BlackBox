"""Exercise the smoke runner with a controlled child, not a native OS claim."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "smoke", Path(__file__).resolve().parents[2] / "scripts/smoke-native-package.py")
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)
REVISION = "a" * 40


class NativePackageSmokeTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="blackbox-smoke-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.report = self.root / "report.ini"
        self.valid = (f"platform=macOS\nsource_revision={REVISION}\n"
                      "completed=1\nfailed_samples=0\ncollections=6\n")

    def test_report_rejects_incomplete_wrong_identity_and_duplicate_fields(self):
        invalid = [
            self.valid.replace("completed=1", "completed=0"),
            self.valid.replace(REVISION, "b" * 40),
            self.valid.replace("macOS", "Linux"),
            self.valid.replace("failed_samples=0", "failed_samples=1"),
            self.valid.replace("collections=6", "collections=0"),
            self.valid.replace("collections=6", "collections=garbage"),
            self.valid + "completed=1\n", "", "invalid\n",
        ]
        for content in invalid:
            with self.subTest(content=content):
                self.report.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    SMOKE.verify_report(self.report, "macOS", REVISION)

    def child(self, body):
        script = self.root / "child.py"
        script.write_text("import sys, os, time\nfrom pathlib import Path\n" + body,
                          encoding="utf-8")
        return [sys.executable, str(script)]

    def test_launch_uses_isolated_state_and_accepts_completed_report(self):
        command = self.child(
            "assert Path(os.environ['BLACKBOX_SETTINGS_PATH']).parent == Path.cwd()\n"
            "assert Path(os.environ['BLACKBOX_PRODUCT_SETTINGS_PATH']).parent == Path.cwd()\n"
            "assert not Path(os.environ['BLACKBOX_SETTINGS_PATH']).exists()\n"
            "report = Path(next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--diagnostic-report=')))\n"
            f"report.write_text({self.valid!r}, encoding='utf-8')\n")
        evidence = self.root / "unicode é 测试"
        SMOKE.run_smoke(command, evidence, "macOS", REVISION)
        self.assertTrue(json.loads((evidence / "result.json").read_text())["completed"])

    def test_timeout_terminates_child_and_retains_failure(self):
        command = self.child("print('child started', flush=True)\ntime.sleep(60)\n")
        evidence = self.root / "timeout"
        with self.assertRaises(subprocess.TimeoutExpired):
            SMOKE.run_smoke(command, evidence, "macOS", REVISION, timeout=0.5)
        self.assertFalse(json.loads((evidence / "result.json").read_text())["completed"])
        self.assertIn("child started", (evidence / "application.log").read_text())

    def test_failed_child_and_missing_report_cannot_pass(self):
        for exit_code in (0, 2):
            with self.subTest(exit_code=exit_code):
                command = self.child(f"sys.exit({exit_code})\n")
                with self.assertRaises((OSError, subprocess.CalledProcessError)):
                    SMOKE.run_smoke(command, self.root / str(exit_code), "macOS", REVISION)

    def test_existing_evidence_is_rejected_without_overwriting(self):
        evidence = self.root / "existing"
        evidence.mkdir()
        existing = evidence / "runtime.ini"
        existing.write_text(self.valid, encoding="utf-8")
        with self.assertRaises(FileExistsError):
            SMOKE.run_smoke(["unused"], evidence, "macOS", REVISION)
        self.assertEqual(existing.read_text(), self.valid)


if __name__ == "__main__":
    unittest.main()

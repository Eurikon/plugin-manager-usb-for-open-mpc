import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("release_archive", ROOT / "tools/release_archive.py")
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)
BUNDLE = os.environ.get("TEST_MANAGER_BUNDLE", "")


@unittest.skipUnless(BUNDLE, "Supply TEST_MANAGER_BUNDLE with a fresh signed manager bundle")
class ReleaseArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / "bundle"
        shutil.copytree(BUNDLE, self.bundle)
        self.version = json.loads((self.bundle / "PMU-UPDATE.json").read_text())["generation"]["version"]
        self.archive = self.root / ("plugin-manager-usb-v" + self.version + ".zip")
        core_spec = importlib.util.spec_from_file_location("release_test_core_inputs", ROOT / "tests/run_update_core.py")
        core = importlib.util.module_from_spec(core_spec)
        core_spec.loader.exec_module(core)
        self.lifecycle = {}
        for arch in RELEASE.ARCHITECTURES:
            directory = self.root / arch
            directory.mkdir()
            log = directory / "tests.log"
            log.write_text("".join("PASS: update-return-reopen " + medium + " with concurrent helper confirmation\n" for medium in ("sd", "usb")))
            report = directory / "results.json"
            report.write_text(json.dumps({"arch": arch, "passed": True, "returncode": 0,
                "update_lifecycle": {"sd": True, "usb": True}, "checks_by_suite": {"runtime-handoff": 2},
                "files": {name: RELEASE.digest(ROOT / name) for name in core.SOURCES},
                "tests_sha256": RELEASE.digest(log)}))
            self.lifecycle[arch] = report

    def receipt(self):
        value = {"schema": 1, "version": self.version, "archive_sha256": RELEASE.digest(self.archive),
                 "validators": {arch: {} for arch in RELEASE.ARCHITECTURES},
                 "lifecycle": RELEASE.validate_lifecycle(self.lifecycle)}
        RELEASE.receipt_path(self.archive).write_text(json.dumps(value))

    def test_archive_has_only_signed_files_and_envelope(self):
        RELEASE.write_archive(self.bundle, self.archive)
        expected = set(json.loads((self.bundle / "PMU-UPDATE.json").read_text())["generation"]["files"]) | {"PMU-UPDATE.json"}
        with zipfile.ZipFile(self.archive) as archive:
            self.assertEqual(set(archive.namelist()), expected)
            self.assertFalse(any(member.is_dir() for member in archive.infolist()))
            self.assertIsNone(archive.testzip())

    def test_uninventoried_file_prevents_archiving(self):
        (self.bundle / "unexpected.txt").write_text("unrelated data")
        with self.assertRaisesRegex(ValueError, "uninventoried"):
            RELEASE.write_archive(self.bundle, self.archive)
        self.assertFalse(self.archive.exists())

    def test_changed_payload_prevents_archiving(self):
        (self.bundle / "autorun/open.sh").write_text("changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            RELEASE.write_archive(self.bundle, self.archive)

    def test_used_local_folder_prevents_archiving(self):
        (self.bundle / "local-plugins/user-data.txt").write_text("preserve user data")
        with self.assertRaises(ValueError):
            RELEASE.write_archive(self.bundle, self.archive)

    def test_missing_receipt_prevents_publication(self):
        RELEASE.write_archive(self.bundle, self.archive)
        with patch.object(RELEASE, "run") as command:
            with self.assertRaises(FileNotFoundError):
                RELEASE.publish(self.archive, "OWNER/REPO", self.root / "notes")
            command.assert_not_called()

    def test_changed_zip_prevents_publication(self):
        RELEASE.write_archive(self.bundle, self.archive)
        self.receipt()
        with self.archive.open("ab") as archive:
            archive.write(b"changed")
        with patch.object(RELEASE, "run") as command:
            with self.assertRaisesRegex(ValueError, "matching"):
                RELEASE.publish(self.archive, "OWNER/REPO", self.root / "notes")
            command.assert_not_called()

    def test_failed_native_check_prevents_publication(self):
        RELEASE.write_archive(self.bundle, self.archive)
        self.receipt()
        with patch.object(RELEASE, "validate", side_effect=ValueError("Baseline refused ZIP")), patch.object(RELEASE, "run") as command:
            with self.assertRaisesRegex(ValueError, "Baseline refused"):
                RELEASE.publish(self.archive, "OWNER/REPO", self.root / "notes")
            command.assert_not_called()

    def test_failed_preparation_does_not_create_acceptance_receipt(self):
        validators = {arch: ROOT / "tools/release_archive.py" for arch in RELEASE.ARCHITECTURES}
        with patch.object(RELEASE, "run", return_value="sha256:" + "1" * 64), patch.object(RELEASE, "validate", side_effect=ValueError("Baseline refused ZIP")):
            with self.assertRaisesRegex(ValueError, "Baseline refused"):
                RELEASE.prepare(self.bundle, self.archive, validators, lifecycle_reports=self.lifecycle)
        self.assertFalse(RELEASE.receipt_path(self.archive).exists())

    def test_missing_lifecycle_reports_prevent_archiving(self):
        with self.assertRaisesRegex(ValueError, "reports are required"):
            RELEASE.prepare(self.bundle, self.archive, {})
        self.assertFalse(self.archive.exists())

    def test_stale_lifecycle_sources_prevent_archiving(self):
        path = self.lifecycle["armv7"]
        value = json.loads(path.read_text())
        value["files"]["src/runtime_handoff.cpp"] = "0" * 64
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "stale"):
            RELEASE.prepare(self.bundle, self.archive, {}, lifecycle_reports=self.lifecycle)
        self.assertFalse(self.archive.exists())

    def test_incomplete_lifecycle_prevents_archiving(self):
        path = self.lifecycle["aarch64"]
        value = json.loads(path.read_text())
        value["update_lifecycle"]["sd"] = False
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "failed"):
            RELEASE.prepare(self.bundle, self.archive, {}, lifecycle_reports=self.lifecycle)
        self.assertFalse(self.archive.exists())

    def test_changed_lifecycle_report_prevents_publication(self):
        RELEASE.write_archive(self.bundle, self.archive)
        self.receipt()
        with self.lifecycle["armv7"].open("a") as report:
            report.write("\n")
        with patch.object(RELEASE, "run") as command:
            with self.assertRaisesRegex(ValueError, "Lifecycle report changed"):
                RELEASE.publish(self.archive, "OWNER/REPO", self.root / "notes")
            command.assert_not_called()

    def test_changed_lifecycle_log_prevents_publication(self):
        RELEASE.write_archive(self.bundle, self.archive)
        self.receipt()
        (self.lifecycle["aarch64"].parent / "tests.log").write_text("changed")
        with patch.object(RELEASE, "run") as command:
            with self.assertRaisesRegex(ValueError, "Lifecycle test log changed"):
                RELEASE.publish(self.archive, "OWNER/REPO", self.root / "notes")
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()

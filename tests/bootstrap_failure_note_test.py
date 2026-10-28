import os
import json
import shlex
import time
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / "src/startup/bootstrap.sh").read_text()
FUNCTIONS = re.search(r"^failure_note\(\) \{.*?^fail\(\) \{[^\n]*\n", SCRIPT, re.S | re.M).group(0)


class FailureNoteTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.media = Path(self.temporary.name) / "media"
        self.usb = self.media / "STICK"
        self.usb.mkdir(parents=True)
        self.helper = Path(self.temporary.name) / "helper"
        self.helper.mkdir()
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        *shlex.split(os.environ.get("CFLAGS", "")), "-DPMU_NOTE_MEDIA=" + json.dumps(str(self.media)),
                        str(ROOT / "src/startup/bootstrap_note.c"), "-o", str(self.helper / "bootstrap-note")],
                       check=True, capture_output=True, text=True)

    def run_fail(self, volume, message="runtime checksum mismatch", code=11, identity=None):
        marker = Path(volume) / ".mpc-manager-id" if volume else None
        if identity is None:
            if marker is not None and marker.exists():
                directory, token = Path(volume).lstat(), marker.lstat()
                identity = f"{directory.st_dev}:{directory.st_ino}:{token.st_dev}:{token.st_ino}"
            else:
                identity = ""
        values = {"SOURCE": str(self.helper), "NOTE_WRITER": "bootstrap-note", "confirmed_usb": str(volume),
                  "confirmed_identity": identity}
        program = "set -eu\n" + "".join(name + "=" + shlex.quote(value) + "\n" for name, value in values.items())
        program += 'regular_source() { [ -f "$1" ] && [ ! -L "$1" ]; }\n' + FUNCTIONS
        program += "\nfail " + shlex.quote(message) + " " + str(code) + "\n"
        return subprocess.run(["sh", "-c", program], capture_output=True, text=True, env={"PATH": os.environ["PATH"]})

    def test_reason_is_written_to_a_marked_stick(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        result = self.run_fail(self.usb)
        self.assertEqual(result.returncode, 11)
        self.assertEqual(result.stderr, "runtime checksum mismatch\n")
        note = (self.usb / "bootstrap-result.txt").read_text()
        self.assertIn("(exit 11): runtime checksum mismatch", note)

    def test_second_failure_appends(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        self.run_fail(self.usb)
        self.run_fail(self.usb, "invalid USB marker", 11)
        self.assertEqual(len((self.usb / "bootstrap-result.txt").read_text().splitlines()), 2)

    def test_nothing_is_written_without_the_marker_or_a_volume(self):
        for volume in (self.usb, "", self.media / "other" / "deep"):
            result = self.run_fail(volume)
            self.assertEqual(result.returncode, 11)
        self.assertEqual(list(self.usb.iterdir()), [])

    def test_a_symlinked_result_file_is_not_followed(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        target = Path(self.temporary.name) / "target"
        (self.usb / "bootstrap-result.txt").symlink_to(target)
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertFalse(target.exists())

    def test_oversized_result_file_is_left_alone(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        big = self.usb / "bootstrap-result.txt"
        big.write_bytes(b"x" * 70000)
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertEqual(big.stat().st_size, 70000)

    def test_record_that_does_not_fit_is_not_appended(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        note = self.usb / "bootstrap-result.txt"
        note.write_bytes(b"x" * 65535)
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertEqual(note.stat().st_size, 65535)

    def test_record_fits_exactly_at_the_limit(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        note = self.usb / "bootstrap-result.txt"
        self.run_fail(self.usb)
        length = note.stat().st_size
        note.write_bytes(b"x" * (65536 - length))
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertEqual(note.stat().st_size, 65536)
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertEqual(note.stat().st_size, 65536)

    def test_replaced_volume_identity_is_rejected(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        self.assertEqual(self.run_fail(self.usb, identity="0:0:0:0").returncode, 11)
        self.assertFalse((self.usb / "bootstrap-result.txt").exists())

    def test_symlinked_volume_and_marker_are_rejected(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        alias = self.media / "ALIAS"
        alias.symlink_to(self.usb, target_is_directory=True)
        self.assertEqual(self.run_fail(alias).returncode, 11)
        target = Path(self.temporary.name) / "marker"
        target.write_text("x")
        (self.usb / ".mpc-manager-id").unlink()
        (self.usb / ".mpc-manager-id").symlink_to(target)
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertFalse((self.usb / "bootstrap-result.txt").exists())

    def test_hardlinked_result_is_not_modified(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        target = Path(self.temporary.name) / "user-file"
        target.write_text("preserve")
        os.link(target, self.usb / "bootstrap-result.txt")
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertEqual(target.read_text(), "preserve")

    def test_fifo_does_not_block_the_original_failure(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        os.mkfifo(self.usb / "bootstrap-result.txt")
        started = time.monotonic()
        self.assertEqual(self.run_fail(self.usb).returncode, 11)
        self.assertLess(time.monotonic() - started, 1)

    def test_unwritable_stick_still_exits_with_the_original_code(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        (self.usb / "bootstrap-result.txt").mkdir()
        self.assertEqual(self.run_fail(self.usb).returncode, 11)


if __name__ == "__main__":
    unittest.main()

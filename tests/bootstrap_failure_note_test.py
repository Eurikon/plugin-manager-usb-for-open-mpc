import os
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

    def run_fail(self, volume, message="runtime checksum mismatch", code=11):
        program = f"set -eu\nMEDIA='{self.media}'\nconfirmed_usb='{volume}'\n{FUNCTIONS}\nfail '{message}' {code}\n"
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

    def test_unwritable_stick_still_exits_with_the_original_code(self):
        (self.usb / ".mpc-manager-id").write_text("x")
        (self.usb / "bootstrap-result.txt").mkdir()
        self.assertEqual(self.run_fail(self.usb).returncode, 11)


if __name__ == "__main__":
    unittest.main()

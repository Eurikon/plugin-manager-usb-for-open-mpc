import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / "src/startup/bootstrap.sh").read_text()
FUNCTION = re.search(r"^running_mpc\(\) \{.*?^\}\n", SCRIPT, re.S | re.M).group(0)


class RunningMpcTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.proc = self.root / "proc"
        self.proc.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()

    def fake_pidof(self, output):
        script = self.bin / "pidof"
        script.write_text("#!/bin/sh\n" + (f"echo '{output}'\n" if output is not None else "exit 1\n"))
        script.chmod(script.stat().st_mode | stat.S_IXUSR)

    def process(self, pid, target="firmware"):
        (self.proc / str(pid)).mkdir()
        (self.proc / str(pid) / "exe").symlink_to(self.root / target)
        (self.root / target).write_text("mpc")

    def run_function(self, owner=None):
        owner = os.getuid() if owner is None else owner
        program = f"set -eu\nPROC='{self.proc}'\nOWNER_UID={owner}\n{FUNCTION}\nrunning_mpc\n"
        env = {"PATH": f"{self.bin}:/usr/bin:/bin"}
        return subprocess.run(["sh", "-c", program], capture_output=True, text=True, env=env)

    def test_the_running_executable_is_used(self):
        self.fake_pidof("4242")
        self.process(4242)
        result = self.run_function()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), f"{self.proc}/4242/exe")

    def test_no_running_mpc_returns_failure_so_the_file_on_disk_is_used(self):
        self.fake_pidof(None)
        self.assertNotEqual(self.run_function().returncode, 0)

    def test_the_first_usable_process_wins_and_junk_is_skipped(self):
        self.fake_pidof("abc 7 99")
        self.process(99)
        result = self.run_function()
        self.assertEqual(result.stdout.strip(), f"{self.proc}/99/exe")

    def test_a_process_owned_by_someone_else_is_ignored(self):
        self.fake_pidof("5")
        self.process(5)
        self.assertNotEqual(self.run_function(owner=os.getuid() + 1).returncode, 0)

    def test_the_check_uses_the_function_with_a_fallback(self):
        self.assertIn('firmware=$(running_mpc) || firmware=$MPC', SCRIPT)
        self.assertIn('sha256sum "$firmware"', SCRIPT)
        self.assertNotIn('sha256sum "$MPC"', SCRIPT)


if __name__ == "__main__":
    unittest.main()

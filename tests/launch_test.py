import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="manager launch ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.autorun = self.root / "stick/autorun"
        self.runtime = self.autorun / "runtime"
        self.runtime.mkdir(parents=True)
        shutil.copyfile(SOURCE / "open.sh", self.autorun / "open.sh")
        self.commands = self.root / "bin"
        self.commands.mkdir()
        self.work = self.root / "RAM"
        self.record = self.root / "selected"
        self.env = dict(os.environ, PATH=str(self.commands) + os.pathsep + os.environ["PATH"],
                        LAUNCH_WORK=str(self.work), LAUNCH_RECORD=str(self.record), LAUNCH_ARCH="aarch64")
        self.command("uname", 'printf "%s\\n" "$LAUNCH_ARCH"\n')
        self.command("mktemp", 'mkdir "$LAUNCH_WORK"\nprintf "%s\\n" "$LAUNCH_WORK"\n')
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            (self.runtime / name).write_text("armv7:" + name)
            (self.runtime / "aarch64").mkdir(exist_ok=True)
            (self.runtime / "aarch64" / name).write_text("aarch64:" + name)
        (self.runtime / "font.ttf").write_text("shared font")
        (self.runtime / "setup.sh").write_text('''#!/bin/sh
set -eu
for name in manager shortcut shortcut-hook.so font.ttf; do cat "$2/$name"; printf '\\n'; done > "$LAUNCH_RECORD"
[ ! -e "$2/aarch64" ]
''')

    def command(self, name, body):
        path = self.commands / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o700)

    def invoke(self):
        return subprocess.run(["sh", str(self.autorun / "open.sh")], env=self.env,
                              text=True, capture_output=True, timeout=5)

    def test_aarch64_selects_all_native_files_and_shared_assets(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.record.read_text().splitlines(),
                         ["aarch64:manager", "aarch64:shortcut", "aarch64:shortcut-hook.so", "shared font"])

    def test_armv7_keeps_legacy_runtime(self):
        self.env["LAUNCH_ARCH"] = "armv7l"
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.record.read_text().splitlines(),
                         ["armv7:manager", "armv7:shortcut", "armv7:shortcut-hook.so", "shared font"])

    def test_unsupported_architecture_stops_before_staging(self):
        self.env["LAUNCH_ARCH"] = "x86_64"
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertFalse(self.work.exists())
        self.assertFalse(self.record.exists())

    def test_missing_arm64_runtime_stops_before_staging(self):
        shutil.rmtree(self.runtime / "aarch64")
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertFalse(self.work.exists())
        self.assertFalse(self.record.exists())

    def test_incomplete_arm64_runtime_is_not_dispatched(self):
        (self.runtime / "aarch64/shortcut").unlink()
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertFalse(self.record.exists())
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SETUP = Path(__file__).resolve().parents[1] / "setup.sh"


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="manager setup ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.usb = self.root / "USB stick"
        self.usb.mkdir()
        self.work = self.root / "RAM session"
        self.work.mkdir()
        self.dispatch = self.root / "dispatch"
        self.calls = self.root / "services"
        self.ports = self.root / "ports"
        self.environment = self.root / "environment"
        self.ports.write_text("Dir Device Name\nIO hw:1,0,1 MPC Private\n")
        self.environment.write_text("Environment=\nEnvironmentFiles=\n")
        self.env = os.environ.copy()
        self.env.pop("MPC_MANAGER_SD_HELPER", None)
        self.env.update({
            "PATH": str(self.bin) + os.pathsep + self.env["PATH"],
            "SETUP_PORTS": str(self.ports),
            "SETUP_ENVIRONMENT": str(self.environment),
            "SETUP_DISPATCH": str(self.dispatch),
            "SETUP_CALLS": str(self.calls),
        })
        self.mock("id", '[ "$#:$1" = "1:-u" ] || exit 90\nprintf "%s\\n" "${SETUP_UID-0}"\n')
        self.mock("systemctl", '''printf '%s\n' "$*" >> "$SETUP_CALLS"
case "$1" in
    cat) exit "${SETUP_CAT_STATUS-0}" ;;
    show) cat "$SETUP_ENVIRONMENT" ;;
    *) exit 91 ;;
esac
''')
        self.mock("amidi", '''[ "$#:$1" = '1:-l' ] || exit 92
cat "$SETUP_PORTS"
exit "${SETUP_AMIDI_STATUS-0}"
''')
        self.mock("systemd-run", '''printf '%s\n' "$@" > "$SETUP_DISPATCH"
exit "${SETUP_DISPATCH_STATUS-0}"
''')

    def mock(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def run_setup(self, *extra):
        return subprocess.run(
            ["/bin/sh", str(SETUP), str(self.usb), str(self.work), *extra],
            env=self.env, text=True, capture_output=True, timeout=5,
        )

    def assert_dispatch(self, result, port="hw:1,0,1", service="acvs", extra=(), environment=()):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.dispatch.read_text().splitlines(), [
            "--unit=mpc-usb-manager", "--collect", *environment, str(self.work / "shortcut"),
            str(self.usb), str(self.work), service, port, *extra,
        ])
        self.assertTrue(self.work.is_dir())
        self.assertEqual(self.calls.read_text().splitlines(), [
            "cat acvs",
        ])

    def assert_rejected(self, result, message=None):
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.dispatch.exists())
        self.assertFalse(self.work.exists())
        if message:
            self.assertIn(message, result.stdout)
        if self.calls.exists():
            self.assertTrue(all(line.startswith(("cat ", "show "))
                                for line in self.calls.read_text().splitlines()))

    def test_stock_mpc_bidirectional_port(self):
        self.assert_dispatch(self.run_setup())

    def test_stopped_mpc_resume_dispatch(self):
        self.assert_dispatch(self.run_setup("--resume-only"),extra=("--resume-only",))

    def test_force_input_port(self):
        self.ports.write_text("Dir Device Name\nI hw:2,3,4 Akai Pro Force Private\n")
        self.assert_dispatch(self.run_setup(), port="hw:2,3,4")

    def test_whitespace_and_other_midi_ports(self):
        self.ports.write_text("""Dir     Device    Name
O  hw:0,0,0 MPC Private
IO hw:0,1,0 USB MIDI
\tIO \t hw:12,34,56 \t CustomPrivate \t
""")
        self.assert_dispatch(self.run_setup(), port="hw:12,34,56")

    def test_private_name_must_be_suffix_on_input_row(self):
        cases = [
            "", "Dir Device Name\n", "O hw:1,0,1 MPC Private\n",
            "IO hw:1,0,1 PrivateElsewhere\n",
            "IO hw:1,0,1 MPC Private Extra\n",
            "X hw:1,0,1 MPC Private\n",
            "Warning hw:1,0,1 MPC Private\n",
            "IO default MPC Private\n",
            "IO hw:card,0,1 MPC Private\n",
            "IO hw:1,0 MPC Private\n",
            "IO hw:1,0,1;invalid MPC Private\n",
        ]
        for content in cases:
            with self.subTest(content=content):
                if not self.work.exists():
                    self.work.mkdir()
                self.ports.write_text(content)
                self.assert_rejected(self.run_setup(), "not found or ambiguous")

    def test_multiple_eligible_ports_fail_closed(self):
        for second in ("I hw:2,0,1 Akai Pro Force Private", "IO hw:1,0,1 MPC Private"):
            with self.subTest(second=second):
                if not self.work.exists():
                    self.work.mkdir()
                self.ports.write_text("IO hw:1,0,1 MPC Private\n" + second + "\n")
                self.assert_rejected(self.run_setup(), "ambiguous")

    def test_existing_preload_reaches_supervisor_without_evaluation(self):
        content = 'Environment="LD_PRELOAD=/first.so /second.so:$(touch never-run)"\nEnvironmentFiles=\n'
        self.environment.write_text(content)
        self.assert_dispatch(self.run_setup())
        self.assertEqual(self.environment.read_text(), content)
        self.assertFalse((self.root / "never-run").exists())

    def test_environment_files_reach_supervisor_unchanged(self):
        content = 'Environment=\nEnvironmentFiles="/existing addin.env" (ignore_errors=no)\n'
        self.environment.write_text(content)
        self.assert_dispatch(self.run_setup())
        self.assertEqual(self.environment.read_text(), content)

    def test_arm_only_option_preserves_arguments(self):
        self.assert_dispatch(self.run_setup("--arm-only"), extra=("--arm-only",))

    def test_sd_helper_forwarded_without_splitting(self):
        helper = "/sdcard/MPC Documents/Devices/Plugin Manager helper"
        self.env["MPC_MANAGER_SD_HELPER"] = helper
        self.assert_dispatch(self.run_setup("--arm-only"), extra=("--arm-only",),
                             environment=("--setenv=MPC_MANAGER_SD_HELPER=" + helper,))

    def test_other_sd_helper_refused(self):
        self.env["MPC_MANAGER_SD_HELPER"] = "/media/untrusted/helper"
        self.assert_rejected(self.run_setup(), "Unsupported SD installation helper")

    def test_unknown_option_and_extra_argument_refused(self):
        for extra in (("--unknown",), ("--arm-only", "extra")):
            with self.subTest(extra=extra):
                if not self.work.exists():
                    self.work.mkdir()
                self.assert_rejected(self.run_setup(*extra), "Expected USB RAM_SESSION")
                self.assertFalse(self.calls.exists())

    def test_unprivileged_execution_refused(self):
        self.env["SETUP_UID"] = "501"
        self.assert_rejected(self.run_setup())
        self.assertFalse(self.calls.exists())

    def test_service_fallback(self):
        self.env["SETUP_CAT_STATUS"] = "1"
        self.assert_dispatch(self.run_setup(), service="inmusic-mpc")

    def test_failed_enumeration_refused_even_with_valid_output(self):
        self.env["SETUP_AMIDI_STATUS"] = "1"
        self.assert_rejected(self.run_setup(), "MIDI port enumeration failed")

    def test_failed_dispatch_removes_ram_session(self):
        self.env["SETUP_DISPATCH_STATUS"] = "1"
        result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.dispatch.exists())
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()

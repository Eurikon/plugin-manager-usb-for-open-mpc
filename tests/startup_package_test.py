import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HELPER = load("startup_helper_test_module", ROOT / "tools/helper_package.py")
PACKAGE = load("startup_package_test_module", ROOT / "package.py")


def project(samples=None, sequences=None):
    data = {"data": {"samples": samples or [], "sequences": sequences or []}}
    return gzip.compress(b"ACVS\n3.10\nSerialisableProjectData\njson\nLinux\n" + json.dumps(data).encode(), mtime=0)


class StartupPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "src/startup"
        self.source.mkdir(parents=True)
        for name in ("plugin_manager.lua", "bootstrap.sh"):
            shutil.copyfile(ROOT / "src/startup" / name, self.source / name)
        self.runtime = {}
        for name in HELPER.RUNTIME:
            data = ("runtime input " + name).encode()
            self.runtime[name] = data
            path = self.root / (name if name in ("launch.sh", "setup.sh") else "build/" + name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for name in ("font-license.txt", "json-license.txt"):
            (self.root / "build" / name).write_text("notice")
        (self.root / "licenses").mkdir()
        (self.root / "licenses/SDL2.txt").write_text("notice")
        for name in ("open.sh", "LICENSE"):
            (self.root / name).write_text("input " + name)
        module = bytearray(52)
        module[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<H", module, 18, 40)
        (self.root / "build/startup").mkdir()
        self.module = self.root / "build/startup/manager-entry.so"
        self.module.write_bytes(module)
        self.project = self.root / "blank.xpj"
        self.project.write_bytes(project())
        self.seed_reader = HELPER.profile_seed
        for target, name, value in ((HELPER, "PROJECT", self.root), (HELPER, "SOURCE", self.source),
                                    (HELPER, "profile_seed", lambda _: b"controller checksum fixture"),
                                    (PACKAGE, "PROJECT", self.root), (PACKAGE, "_helper", lambda: HELPER),
                                    (PACKAGE, "validate_mpc_executable", lambda _: (b"autorun checksum fixture", {}))):
            context = patch.object(target, name, value)
            context.start()
            self.addCleanup(context.stop)

    def archive(self):
        return HELPER.build_archive(self.root / "external-MPC", self.project)

    def altered_archive(self, name, value, *, extra=False, mode=None):
        original = self.archive()
        output = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(original)) as source, zipfile.ZipFile(output, "w") as destination:
            for entry in source.infolist():
                data = source.read(entry.filename)
                if entry.filename == HELPER.PREFIX + name and not extra:
                    data = value
                    if mode is not None:
                        entry.external_attr = mode << 16
                destination.writestr(entry, data)
            if extra:
                if HELPER.PREFIX + name in source.namelist():
                    with self.assertWarns(UserWarning):
                        destination.writestr(HELPER.PREFIX + name, value)
                else:
                    destination.writestr(HELPER.PREFIX + name, value)
        return output.getvalue()

    def test_blank_project_is_accepted_unchanged(self):
        self.assertEqual(HELPER.setup_project(self.project.read_bytes()), self.project.read_bytes())

    def test_nonblank_project_is_rejected(self):
        for data in (project(samples=["external sample"]), project(sequences=[{"events": [1]}])):
            with self.subTest(), self.assertRaises(ValueError):
                HELPER.setup_project(data)

    def test_truncated_or_trailing_project_is_rejected(self):
        for data in (self.project.read_bytes()[:-4], self.project.read_bytes() + b"trailing"):
            with self.subTest(), self.assertRaises(ValueError):
                HELPER.setup_project(data)

    def test_project_decompression_is_bounded(self):
        with self.assertRaises(ValueError):
            HELPER.setup_project(gzip.compress(b"x" * (8 * 1024 * 1024 + 1)))

    def test_wrong_project_format_is_rejected(self):
        for data in (gzip.compress(b"wrong"), gzip.compress(b"ACVS\n3.10\nSerialisableProjectData\njson\nLinux\n{}")):
            with self.subTest(), self.assertRaises(ValueError):
                HELPER.setup_project(data)

    def test_unknown_firmware_refuses_profile_generation(self):
        binary = self.root / "unknown-MPC"
        binary.write_bytes(b"unsupported firmware")
        with self.assertRaisesRegex(ValueError, "tested stock"):
            self.seed_reader(binary)

    def test_helper_archive_is_deterministic_and_runtime_bound(self):
        first = self.archive()
        self.assertEqual(first, self.archive())
        files = HELPER.validate_archive(first, self.runtime)
        self.assertEqual(files["runtime.sha256"], HELPER.runtime_manifest(self.runtime))
        self.assertEqual(files["PluginManager.xpj"], self.project.read_bytes())
        self.assertTrue(files["plugin_manager.lua"].startswith(b"local ENABLED = true\n"))
        self.assertIn(hashlib.sha256(files["plugin_manager.lua"] + b"controller checksum fixture").hexdigest().encode(),
                      files["controller.ini"])

    def test_changed_runtime_is_rejected(self):
        changed = dict(self.runtime, shortcut=b"different")
        with self.assertRaises(ValueError):
            HELPER.validate_archive(self.archive(), changed)

    def test_duplicate_or_unexpected_members_are_rejected(self):
        for name in ("controller.ini", "../unexpected"):
            with self.subTest(), self.assertRaises(ValueError):
                HELPER.validate_archive(self.altered_archive(name, b"extra", extra=True), self.runtime)

    def test_symlink_member_is_rejected(self):
        with self.assertRaises(ValueError):
            HELPER.validate_archive(self.altered_archive("manager-entry.so", b"outside", mode=stat.S_IFLNK | 0o777), self.runtime)

    def test_project_cleanup_mismatch_is_rejected(self):
        changed = gzip.compress(gzip.decompress(self.project.read_bytes()).replace(b"3.10", b"3.10.1"), mtime=0)
        with self.assertRaises(ValueError):
            HELPER.validate_archive(self.altered_archive("PluginManager.xpj", changed), self.runtime)

    def test_unsupported_module_is_rejected(self):
        self.module.write_bytes(b"not an ARM module")
        with self.assertRaises(ValueError):
            self.archive()

    def test_universal_manifest_covers_helper_without_plugin_payload(self):
        output = self.root / "bundle"
        PACKAGE.build_universal_package(self.root / "legacy-MPC", self.root / "new-MPC", self.project, output)
        manifest = json.loads((output / "USB-SHA256.json").read_text())
        self.assertEqual(manifest[HELPER.INSTALLER], hashlib.sha256((output / HELPER.INSTALLER).read_bytes()).hexdigest())
        self.assertEqual(list((output / "local-plugins").iterdir()), [])

    def test_failed_universal_build_leaves_destination_absent(self):
        self.module.write_bytes(b"invalid")
        output = self.root / "bundle"
        with self.assertRaises(ValueError):
            PACKAGE.build_universal_package(self.root / "legacy-MPC", self.root / "new-MPC", self.project, output)
        self.assertFalse(output.exists())

    def test_existing_destination_is_preserved(self):
        output = self.root / "bundle"
        output.mkdir()
        (output / "keep").write_text("user data")
        with self.assertRaises(FileExistsError):
            PACKAGE.build_universal_package(self.root / "legacy-MPC", self.root / "new-MPC", self.project, output)
        self.assertEqual((output / "keep").read_text(), "user data")


if __name__ == "__main__":
    unittest.main()

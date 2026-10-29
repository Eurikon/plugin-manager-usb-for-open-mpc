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
from tools.sign_release import Signer, initialize

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
        (self.root / "build/startup/bootstrap-note").write_bytes(module)
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

    def signed_inputs(self):
        private = self.root / "signing/release.pem"
        public = self.root / "src/publisher_key.hpp"
        initialize(private, public)
        (self.root / "src/version.hpp").write_text('#define MPC_MANAGER_VERSION "1.3.0"\n')
        for architecture in ("armv7", "aarch64"):
            wide = architecture == "aarch64"
            binary = bytearray(64 if wide else 52)
            binary[:7] = b"\x7fELF" + bytes((2 if wide else 1, 1, 1))
            struct.pack_into("<H", binary, 16, 3)
            struct.pack_into("<H", binary, 18, 183 if wide else 40)
            struct.pack_into("<H", binary, 52 if wide else 40, len(binary))
            struct.pack_into("<I", binary, 48 if wide else 36, 0 if wide else 0x05000400)
            build = self.root / "build" / ("aarch64" if wide else "")
            (build / "startup").mkdir(parents=True, exist_ok=True)
            for name in ("manager", "shortcut", "shortcut-hook.so", "source-dispatcher",
                         "startup/manager-entry.so", "startup/manager-loader.so", "startup/bootstrap-note"):
                (build / name).write_bytes(binary)
        return private, Signer(private, public)

    def test_versioned_helper_contains_stable_loaders_and_signed_payload(self):
        _, signer = self.signed_inputs()
        data = HELPER.build_versioned_archive(self.root / "external-MPC", self.project, signer,
                                              "1.3.0", self.root / "external-MPC64")
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            files = {name.removeprefix(HELPER.PREFIX): archive.read(name) for name in archive.namelist()}
        selection = json.loads(files[".pmu-versions/active.json"])
        prefix = ".pmu-versions/" + selection["active"] + "/"
        specification = json.loads(files[prefix + "generation.json"])
        self.assertEqual(hashlib.sha256(files[prefix + "generation.json"]).hexdigest(), selection["active"])
        self.assertEqual(set(specification["entrypoints"]), {"helper_armv7", "helper_aarch64", "dispatcher_armv7", "dispatcher_aarch64", "bootstrap"})
        self.assertIn("manager-entry.so", files)
        self.assertIn("manager-entry-aarch64.so", files)
        self.assertNotIn("runtime.sha256", files)
        self.assertNotIn("bootstrap.sh", files)
        self.assertEqual(json.loads(json.loads(files[prefix + "authorization.json"])["message"])["kind"], "helper")

    def test_signed_universal_bundle_binds_mutable_payloads_separately(self):
        private, _ = self.signed_inputs()
        destination = self.root / "signed-stick"
        with patch.object(PACKAGE, "validate_mpc_executable", lambda _: (b"autorun fixture", {"arch": "armv7"})):
            PACKAGE.build_signed_package(self.root / "external-MPC", self.root / "external-MPC310", self.project,
                                         destination, private, arm64_build=self.root / "build/aarch64",
                                         firmware_310_arm64=self.root / "external-MPC64")
        envelope = json.loads((destination / "PMU-UPDATE.json").read_bytes())
        inventory = envelope["generation"]["files"]
        self.assertEqual(set(inventory), {path.relative_to(destination).as_posix() for path in destination.rglob("*")
                                          if path.is_file()} - {"PMU-UPDATE.json"})
        for name, checksum in inventory.items():
            self.assertEqual(hashlib.sha256((destination / name).read_bytes()).hexdigest(), checksum)
        autorun = destination / "autorun"
        metadata = json.loads((autorun / "autorun.json").read_bytes())["value0"]
        self.assertEqual(metadata["files"], ["runtime/source-dispatcher-aarch64", "runtime/source-dispatcher-armv7"])
        checked = ["autorun.json", "open.sh", *metadata["files"]]
        expected = hashlib.sha256(b"".join((autorun / name).read_bytes() for name in checked) + b"autorun fixture").hexdigest()
        self.assertEqual((autorun / "autorun.check").read_text(), expected)
        self.assertFalse(any((destination / "local-plugins").iterdir()))
        pool = autorun / "runtime/.pmu-versions"
        self.assertTrue((pool / "active.armv7.json").is_file() and (pool / "active.aarch64.json").is_file())
        self.assertEqual(json.loads(json.loads(envelope["authorization"])["message"])["kind"], "update")

    def test_versioned_helper_refuses_the_wrong_native_architecture(self):
        _, signer = self.signed_inputs()
        shutil.copyfile(self.root / "build/manager", self.root / "build/aarch64/startup/manager-loader.so")
        with self.assertRaisesRegex(ValueError, "wrong architecture"):
            HELPER.build_versioned_archive(self.root / "external-MPC", self.project, signer,
                                           "1.3.0", self.root / "external-MPC64")

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

    def test_universal_helper_binds_both_runtime_generations(self):
        runtime64 = self.root / "build/aarch64"
        (runtime64 / "startup").mkdir(parents=True)
        module = bytearray(64)
        module[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<H", module, 18, 183)
        (runtime64 / "startup/manager-entry.so").write_bytes(module)
        (runtime64 / "startup/bootstrap-note").write_bytes(module)
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            (runtime64 / name).write_bytes(("ARM64 " + name).encode())
        data = HELPER.build_archive(self.root / "external-MPC", self.project, self.root / "external-Gen2-MPC")
        selected = HELPER.runtime_files("aarch64")
        files = HELPER.validate_archive(data, self.runtime, selected)
        self.assertEqual(files["runtime-aarch64.sha256"], HELPER.runtime_manifest(selected, "aarch64"))
        self.assertIn(b"  aarch64/manager\n", files["runtime-aarch64.sha256"])
        self.assertEqual(files["manager-entry-aarch64.so"], module)
        with self.assertRaises(ValueError): HELPER.validate_archive(data, self.runtime)
        with self.assertRaises(ValueError): HELPER.validate_archive(data, self.runtime, dict(selected, manager=b"wrong"))

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

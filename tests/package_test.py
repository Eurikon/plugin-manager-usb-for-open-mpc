import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import shutil

SPEC = importlib.util.spec_from_file_location("manager_package", Path(__file__).parents[1] / "package.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
TEST_SALT = b"synthetic-autorun-test-value"
TEST_KEY = (0xc9771b45f9913b87).to_bytes(8, "little")


def executable(salt=TEST_SALT, offsets=(259,), kind=3):
    length = max(512, max(offsets, default=0) + 101)
    data = bytearray(length)
    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16,
                     kind, 40, 1, 0x10080, 52, 0, 0x05000400, 52, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", data, 52, 1, 0, 0x10000, 0x10000, length, length, 5, 4096)
    window = salt.ljust(37, b"\0")
    for offset in offsets:
        data[offset:offset + 37] = bytes(value ^ TEST_KEY[index % 8] for index, value in enumerate(window))
    return bytes(data)


def executable64(salt=TEST_SALT):
    data = bytearray(512)
    data[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, 183, 1, 0x10080, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<II6Q", data, 64, 1, 5, 0, 0x10000, 0x10000, len(data), len(data), 4096)
    data[259:296] = bytes(value ^ TEST_KEY[index % 8] for index, value in enumerate(salt.ljust(37, b"\0")))
    return bytes(data)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "manager checkout"
        (self.project / "build").mkdir(parents=True)
        (self.project / "licenses").mkdir()
        for name in ("manager", "font.ttf", "shortcut", "shortcut-hook.so", "font-license.txt", "json-license.txt"):
            (self.project / "build" / name).write_text("fixture " + name)
        for name in ("launch.sh", "setup.sh", "open.sh", "LICENSE"):
            (self.project / name).write_text("fixture " + name)
        (self.project / "licenses/SDL2.txt").write_text("SDL notice")
        for name in ("libpng.txt", "libjpeg-turbo.txt", "zlib.txt"):
            (self.project / "licenses" / name).write_text("third-party notice " + name)
        (self.project / "licenses/TitilliumWeb.txt").write_text("font notice")
        self.firmware = self.root / "external MPC"
        self.firmware.write_bytes(executable())
        self.output = self.root / "USB output"
        self.addCleanup(patch.stopall)
        patch.object(PACKAGE, "PROJECT", self.project).start()
        patch.object(PACKAGE, "EXPECTED_SALT_SHA256", hashlib.sha256(TEST_SALT).hexdigest()).start()

    def test_aarch64_structure_and_salt_are_validated(self):
        self.firmware.write_bytes(executable64())
        salt, result = PACKAGE.validate_mpc_executable(self.firmware)
        self.assertEqual(salt, TEST_SALT)
        self.assertEqual(result["arch"], "aarch64")
        for offset, value in ((18, 40), (54, 32)):
            data = bytearray(executable64())
            struct.pack_into("<H", data, offset, value)
            self.firmware.write_bytes(data)
            with self.assertRaises(ValueError): PACKAGE.validate_mpc_executable(self.firmware)

    def test_default_contains_manager_not_firmware_or_plugins(self):
        PACKAGE.build_package(self.firmware, self.output)
        self.assertEqual(list((self.output / "local-plugins").iterdir()), [])
        self.assertEqual((self.output / ".mpc-manager-id").read_text(), "mpc-usb-manager-v1\n")
        self.assertFalse(any(path.read_bytes() == self.firmware.read_bytes()
                             for path in self.output.rglob("*") if path.is_file()))
        self.assertTrue((self.output / "autorun/runtime/licenses/plugin-manager-usb.txt").is_file())
        self.assertTrue((self.output / "autorun/runtime/licenses/font-license.txt").is_file())

    def test_checksum_covers_license_and_every_payload(self):
        PACKAGE.build_package(self.firmware, self.output)
        autorun = self.output / "autorun"
        payload = json.loads((autorun / "autorun.json").read_text())["value0"]
        self.assertIn("runtime/licenses/plugin-manager-usb.txt", payload["files"])
        for name in ("libpng.txt", "libjpeg-turbo.txt", "zlib.txt"):
            self.assertIn("runtime/licenses/" + name, payload["files"])
            self.assertEqual((autorun / "runtime/licenses" / name).read_text(), "third-party notice " + name)
        ordered = ["autorun.json", "open.sh"] + payload["files"]
        expected = hashlib.sha256(b"".join((autorun / name).read_bytes() for name in ordered) + TEST_SALT).hexdigest()
        self.assertEqual((autorun / "autorun.check").read_text(), expected)
        manifest = json.loads((self.output / "USB-SHA256.json").read_text())
        files = {str(path.relative_to(self.output)) for path in self.output.rglob("*")
                 if path.is_file() and path.name != "USB-SHA256.json"}
        self.assertEqual(set(manifest), files)
        for name, digest in manifest.items():
            self.assertEqual(hashlib.sha256((self.output / name).read_bytes()).hexdigest(), digest)

    def test_dual_architecture_runtime_is_covered_by_both_inventories(self):
        arm64 = self.root / "ARM64 outputs"
        arm64.mkdir()
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            (arm64 / name).write_bytes(executable64())
        PACKAGE.build_package(self.firmware, self.output, arm64_build=arm64)
        autorun = self.output / "autorun"
        files = json.loads((autorun / "autorun.json").read_text())["value0"]["files"]
        manifest = json.loads((self.output / "USB-SHA256.json").read_text())
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            relative = "runtime/aarch64/" + name
            self.assertIn(relative, files)
            self.assertEqual(manifest["autorun/" + relative], hashlib.sha256(executable64()).hexdigest())
        self.assertEqual((autorun / "runtime/manager").read_text(), "fixture manager")

    def test_wrong_arm64_build_does_not_create_bundle(self):
        arm64 = self.root / "ARM64 outputs"
        arm64.mkdir()
        (arm64 / "manager").write_bytes(executable())
        with self.assertRaisesRegex(ValueError, "ARM64 build"):
            PACKAGE.build_package(self.firmware, self.output, arm64_build=arm64)
        self.assertFalse(self.output.exists())

    def test_explicit_external_zip_is_optional(self):
        source = os.environ.get("TEST_PACKAGE_ZIP")
        if not source:
            self.skipTest("Set TEST_PACKAGE_ZIP to your own trusted plugin ZIP")
        source = Path(source).resolve()
        self.assertTrue(source.is_file(), "TEST_PACKAGE_ZIP does not exist")
        plugin = self.root / "external plugin.zip"
        shutil.copyfile(source, plugin)
        PACKAGE.build_package(self.firmware, self.output, plugin)
        self.assertEqual((self.output / "local-plugins" / plugin.name).read_bytes(), plugin.read_bytes())

    def test_wrong_firmware_leaves_destination_absent(self):
        self.firmware.write_bytes(b"wrong firmware")
        with self.assertRaisesRegex(ValueError, "MPC executable"):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_existing_destination_is_preserved(self):
        self.output.mkdir()
        (self.output / "user-file").write_text("preserve")
        with self.assertRaises(FileExistsError):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertEqual((self.output / "user-file").read_text(), "preserve")

    def test_dangling_destination_symlink_is_preserved(self):
        target = self.root / "not-created"
        self.output.symlink_to(target, target_is_directory=True)
        with self.assertRaises(FileExistsError):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertTrue(self.output.is_symlink())
        self.assertFalse(target.exists())

    def test_relocated_and_duplicate_salts_keep_same_checksum(self):
        PACKAGE.build_package(self.firmware, self.output)
        expected = (self.output / "autorun/autorun.check").read_bytes()
        for offsets in ((391,), (259, 433)):
            with self.subTest(offsets=offsets):
                self.firmware.write_bytes(executable(offsets=offsets))
                output = self.root / ("output-" + str(offsets[0]))
                PACKAGE.build_package(self.firmware, output)
                self.assertEqual((output / "autorun/autorun.check").read_bytes(), expected)
                salt, result = PACKAGE.validate_mpc_executable(self.firmware)
                self.assertEqual(salt, TEST_SALT)
                self.assertEqual(len(result["autorun_salt_locations"]), len(offsets))
                self.assertFalse(result["matches_stock_gen1_reference"])

    def test_full_binary_hash_is_evidence_only(self):
        content = bytearray(executable())
        content[200] = 1
        self.firmware.write_bytes(content)
        salt, result = PACKAGE.validate_mpc_executable(self.firmware)
        self.assertEqual(salt, TEST_SALT)
        self.assertEqual(result["executable_sha256"], hashlib.sha256(content).hexdigest())
        self.assertFalse(result["matches_stock_gen1_reference"])
        PACKAGE.build_package(self.firmware, self.output)

    def test_absent_wrong_and_conflicting_salts_leave_destination_absent(self):
        other = b"different-synthetic-test-value"
        conflict = bytearray(executable(offsets=(259, 433)))
        encoded = bytes(value ^ TEST_KEY[index % 8] for index, value in enumerate(other.ljust(37, b"\0")))
        conflict[433:470] = encoded
        for content in (executable(offsets=()), executable(salt=other), bytes(conflict)):
            with self.subTest(size=len(content)):
                self.firmware.write_bytes(content)
                with self.assertRaises(ValueError):
                    PACKAGE.build_package(self.firmware, self.output)
                self.assertFalse(self.output.exists())

    def test_invalid_elf_never_creates_output(self):
        changes = ((4, b"\x02"), (5, b"\x02"), (7, b"\xff"), (16, struct.pack("<H", 1)),
                   (18, struct.pack("<H", 62)), (20, struct.pack("<I", 2)),
                   (24, struct.pack("<I", 0x20000)), (28, struct.pack("<I", 500)),
                   (32, struct.pack("<I", 500)), (36, struct.pack("<I", 0x05000200)),
                   (40, struct.pack("<H", 51)), (42, struct.pack("<H", 31)),
                   (44, struct.pack("<H", 0)), (48, struct.pack("<H", 1)),
                   (52, struct.pack("<I", 0)), (56, struct.pack("<I", 511)),
                   (60, struct.pack("<I", 0xfffffff0)), (68, struct.pack("<I", 513)),
                   (72, struct.pack("<I", 511)), (76, struct.pack("<I", 4)),
                   (80, struct.pack("<I", 3)))
        for offset, value in changes:
            with self.subTest(offset=offset, value=value):
                content = bytearray(executable())
                content[offset:offset + len(value)] = value
                self.firmware.write_bytes(content)
                with self.assertRaises(ValueError):
                    PACKAGE.build_package(self.firmware, self.output)
                self.assertFalse(self.output.exists())

    def test_executable_and_pie_headers_are_supported(self):
        for kind in (2, 3):
            with self.subTest(kind=kind):
                self.firmware.write_bytes(executable(kind=kind))
                salt, result = PACKAGE.validate_mpc_executable(self.firmware)
                self.assertEqual(salt, TEST_SALT)
                self.assertEqual(result["elf_type"], "ET_EXEC" if kind == 2 else "ET_DYN")

    def test_input_size_bound_is_checked_before_scan(self):
        with patch.object(PACKAGE, "MAX_EXECUTABLE_BYTES", 511), self.assertRaises(ValueError):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_trailer_only_salt_is_rejected(self):
        content = bytearray(executable(offsets=(600,)))
        struct.pack_into("<II", content, 68, 512, 512)
        self.firmware.write_bytes(content)
        with self.assertRaisesRegex(ValueError, "loaded MPC content"):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_compressed_only_salt_is_rejected(self):
        import lzma
        content = bytearray(executable(offsets=()))
        content.extend(lzma.compress(executable()))
        self.firmware.write_bytes(content)
        with self.assertRaisesRegex(ValueError, "loaded MPC content"):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_nonregular_input_is_rejected_without_blocking(self):
        fifo = self.root / "firmware-pipe"
        os.mkfifo(fifo)
        with self.assertRaisesRegex(ValueError, "file type"):
            PACKAGE.build_package(fifo, self.output)
        self.assertFalse(self.output.exists())

    def test_input_changed_after_scanning_is_rejected(self):
        scanner = PACKAGE._scanner()
        original_find = scanner.find_salt

        def changed_input(path, expected_sha256):
            result = original_find(path, expected_sha256)
            data = bytearray(path.read_bytes())
            data[200] ^= 1
            path.write_bytes(data)
            return result

        scanner.find_salt = changed_input
        with patch.object(PACKAGE, "_scanner", return_value=scanner), \
                self.assertRaisesRegex(ValueError, "changed during salt scanning"):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_missing_build_input_leaves_destination_absent(self):
        (self.project / "build/shortcut").unlink()
        with self.assertRaisesRegex(FileNotFoundError, "shortcut"):
            PACKAGE.build_package(self.firmware, self.output)
        self.assertFalse(self.output.exists())

    def test_non_zip_cannot_be_bundled_as_local_plugin(self):
        with self.assertRaisesRegex(ValueError, "ZIP"):
            PACKAGE.build_package(self.firmware, self.output, self.firmware)
        self.assertFalse(self.output.exists())

    def test_cli_documents_optional_plugin(self):
        result = subprocess.run([sys.executable, str(SPEC.origin), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("[plugin]", result.stdout)
        self.assertIn("omitted by default", result.stdout)


if __name__ == "__main__":
    unittest.main()

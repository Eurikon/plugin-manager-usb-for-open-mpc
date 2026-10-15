import hashlib
import importlib.util
import lzma
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from package_test import TEST_SALT, executable

SPEC = importlib.util.spec_from_file_location("manager_firmware",
                                               Path(__file__).resolve().parents[1] / "tools/extract_firmware.py")
FIRMWARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIRMWARE)


def filesystem(blocks=4, shift=0, magic=b"\x53\xef"):
    data = bytearray(4096)
    struct.pack_into("<I", data, 1028, blocks)
    struct.pack_into("<I", data, 1048, shift)
    data[1080:1082] = magic
    return bytes(data)


def image(data=None):
    return b"AZ01fixture" + lzma.compress(filesystem() if data is None else data)


class RootfsTests(unittest.TestCase):
    def test_bounded_stream_ignores_later_sections_without_mutating_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "update.img", root / "rootfs.ext"
            content = image() + b"later sections"
            source.write_bytes(content)
            source.chmod(0o400)
            before = source.stat().st_mtime_ns
            result = FIRMWARE.extract_rootfs(source, output)
            self.assertEqual(result, {"xz_offset": 11, "rootfs_bytes": 4096, "stream_padding_bytes": 0})
            self.assertEqual(output.read_bytes(), filesystem())
            self.assertEqual(source.read_bytes(), content)
            self.assertEqual(source.stat().st_mtime_ns, before)

    def test_header_can_span_decompressor_chunks(self):
        decoder_type = lzma.LZMADecompressor

        class FragmentedDecoder:
            def __init__(self, *args, **kwargs):
                self.decoder = decoder_type(*args, **kwargs)

            @property
            def eof(self):
                return self.decoder.eof

            @property
            def needs_input(self):
                return self.decoder.needs_input

            def decompress(self, data, max_length):
                return self.decoder.decompress(data, max_length=min(max_length, 512))

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "update.img"
            source.write_bytes(image())
            with patch.object(FIRMWARE.lzma, "LZMADecompressor", FragmentedDecoder):
                FIRMWARE.extract_rootfs(source, root / "rootfs.ext")
            self.assertEqual((root / "rootfs.ext").read_bytes(), filesystem())

    def test_format_block_size_length_and_truncation_fail_without_partial_output(self):
        cases = [b"Other format", b"AZ01no compressed filesystem", image(filesystem(magic=b"xx")),
                 image(filesystem(blocks=0)), image(filesystem(blocks=600000)),
                 image(filesystem(blocks=5)), image(filesystem(shift=32)), image()[:-12],
                 image(b"too short"), image(filesystem() + b"nonzero")]
        for content in cases:
            with self.subTest(prefix=content[:16]), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, output = root / "update.img", root / "rootfs.ext"
                source.write_bytes(content)
                with self.assertRaises((ValueError, lzma.LZMAError)):
                    FIRMWARE.extract_rootfs(source, output)
                self.assertFalse(output.exists())
                self.assertEqual(source.read_bytes(), content)

    def test_padding_and_size_bounds(self):
        for padding, valid in ((bytes(1024), True), (bytes(4096), True), (bytes(4097), False)):
            with self.subTest(length=len(padding)), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, output = root / "update.img", root / "rootfs.ext"
                source.write_bytes(image(filesystem() + padding))
                if valid:
                    result = FIRMWARE.extract_rootfs(source, output)
                    self.assertEqual(result["stream_padding_bytes"], len(padding))
                    self.assertEqual(output.stat().st_size, 4096)
                else:
                    with self.assertRaises(ValueError):
                        FIRMWARE.extract_rootfs(source, output)
                    self.assertFalse(output.exists())

    def test_decoder_memory_is_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "update.img", root / "rootfs.ext"
            source.write_bytes(image())
            with patch.object(FIRMWARE, "MAX_XZ_MEMORY", 1024), self.assertRaises(lzma.LZMAError):
                FIRMWARE.extract_rootfs(source, output)
            self.assertFalse(output.exists())

    def test_source_size_and_nonregular_inputs_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "update.img", root / "rootfs.ext"
            source.write_bytes(image())
            with patch.object(FIRMWARE, "MAX_IMAGE_BYTES", source.stat().st_size - 1), \
                    self.assertRaisesRegex(ValueError, "size limit"):
                FIRMWARE.extract_rootfs(source, output)
            self.assertFalse(output.exists())
            pipe = root / "firmware-pipe"
            os.mkfifo(pipe)
            with self.assertRaisesRegex(ValueError, "regular file"):
                FIRMWARE.extract_rootfs(pipe, output)
            self.assertFalse(output.exists())

    def test_source_mutation_or_replacement_removes_partial_output(self):
        decoder_type = lzma.LZMADecompressor
        for change in ("overwrite", "replace", "remove"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, output = root / "update.img", root / "rootfs.ext"
                content = image()
                source.write_bytes(content)

                class ChangedInputDecoder:
                    def __init__(self, *args, **kwargs):
                        self.decoder = decoder_type(*args, **kwargs)
                        self.changed = False

                    @property
                    def eof(self):
                        return self.decoder.eof

                    @property
                    def needs_input(self):
                        return self.decoder.needs_input

                    def decompress(self, data, max_length):
                        result = self.decoder.decompress(data, max_length=max_length)
                        if not self.changed:
                            self.changed = True
                            if change == "overwrite":
                                source.write_bytes(content + b"changed")
                            elif change == "replace":
                                replacement = root / "replacement.img"
                                replacement.write_bytes(content)
                                replacement.replace(source)
                            else:
                                source.unlink()
                        return result

                with patch.object(FIRMWARE.lzma, "LZMADecompressor", ChangedInputDecoder), \
                        self.assertRaisesRegex(ValueError, "changed during extraction"):
                    FIRMWARE.extract_rootfs(source, output)
                self.assertFalse(output.exists())

    def test_existing_or_symlink_destination_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "update.img", root / "rootfs.ext"
            content = image()
            source.write_bytes(content)
            output.write_bytes(b"preserve")
            with self.assertRaises(FileExistsError):
                FIRMWARE.extract_rootfs(source, output)
            self.assertEqual(output.read_bytes(), b"preserve")
            output.unlink()
            output.symlink_to(source)
            with self.assertRaises(FileExistsError):
                FIRMWARE.extract_rootfs(source, output)
            self.assertEqual(source.read_bytes(), content)


class ExtractedRootTests(unittest.TestCase):
    def setUp(self):
        self.expected_salt = patch.object(FIRMWARE.manager_package(), "EXPECTED_SALT_SHA256",
                                          hashlib.sha256(TEST_SALT).hexdigest())
        self.expected_salt.start()
        self.addCleanup(self.expected_salt.stop)

    def prepare(self, root):
        source = root / "update.img"
        source.write_bytes(image())
        self.assertEqual(source.read_bytes(), image())
        return source

    def debugfs(self, executable=executable(), busybox=True, executable_mode=True, exit_code=0):
        def run(command, **kwargs):
            self.assertEqual(command[0], "debugfs")
            self.assertNotIn("chroot", command)
            self.assertEqual(kwargs, {"capture_output": True, "timeout": 180})
            root = Path(shlex.split(command[2])[2])
            self.workfile = Path(command[3])
            self.assertEqual(self.workfile.read_bytes(), filesystem())
            binary = root / "usr/bin"
            binary.mkdir(parents=True)
            if executable is not None:
                (binary / "MPC").write_bytes(executable)
            if busybox:
                (binary / "busybox.nosuid").write_bytes(b"synthetic busybox")
                (binary / "busybox.nosuid").chmod(0o700 if executable_mode else 0o600)
            return subprocess.CompletedProcess(command, exit_code, b"", b"")
        return run

    def test_success_validates_external_executable_and_removes_workfile(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as directory:
            root = Path(directory)
            source, destination = self.prepare(root), root / "extracted root"
            content = source.read_bytes()
            with patch.object(FIRMWARE.shutil, "which", return_value="/usr/sbin/debugfs"), \
                    patch.object(FIRMWARE.subprocess, "run", side_effect=self.debugfs()) as run:
                result = FIRMWARE.extract_firmware(source, destination)
            self.assertTrue(result["firmware_executable_verified"])
            self.assertEqual(result["autorun_salt_sha256"], hashlib.sha256(TEST_SALT).hexdigest())
            self.assertEqual(result["executable_sha256"], hashlib.sha256(executable()).hexdigest())
            self.assertFalse(result["matches_stock_gen1_reference"])
            self.assertEqual(run.call_count, 1)
            self.assertFalse(self.workfile.exists())
            self.assertEqual(source.read_bytes(), content)
            self.assertEqual((destination / "usr/bin/MPC").read_bytes(), executable())
            self.assertFalse((destination / "rootfs.ext").exists())

    def test_failed_dump_or_identity_removes_only_new_root(self):
        cases = [dict(exit_code=1), dict(executable=None), dict(busybox=False),
                 dict(executable_mode=False), dict(executable=b"wrong executable")]
        for options in cases:
            with self.subTest(options=options), tempfile.TemporaryDirectory(dir="/tmp") as directory:
                root = Path(directory)
                source, destination = self.prepare(root), root / "extracted"
                content = source.read_bytes()
                with patch.object(FIRMWARE.shutil, "which", return_value="/usr/sbin/debugfs"), \
                        patch.object(FIRMWARE.subprocess, "run", side_effect=self.debugfs(**options)), \
                        self.assertRaises((ValueError, RuntimeError)):
                    FIRMWARE.extract_firmware(source, destination)
                self.assertFalse(destination.exists())
                self.assertFalse(self.workfile.exists())
                self.assertEqual(source.read_bytes(), content)

    def test_unsafe_existing_and_symlink_destinations_do_not_run_debugfs(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as directory:
            root = Path(directory)
            source = self.prepare(root)
            existing = root / "existing"
            existing.mkdir()
            (existing / "keep").write_bytes(b"preserve")
            link = root / "linked"
            link.symlink_to(existing, target_is_directory=True)
            cases = [Path("/tmp"), Path("/not-temporary/extracted"), existing, link, root / 'bad"quote']
            for destination in cases:
                with self.subTest(destination=destination), patch.object(FIRMWARE.subprocess, "run") as run, \
                        self.assertRaises((ValueError, FileExistsError)):
                    FIRMWARE.extract_firmware(source, destination)
                run.assert_not_called()
            self.assertEqual((existing / "keep").read_bytes(), b"preserve")

    def test_missing_debugfs_or_malformed_image_leaves_no_root(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as directory:
            root = Path(directory)
            source, destination = self.prepare(root), root / "extracted"
            with patch.object(FIRMWARE.shutil, "which", return_value=None), self.assertRaisesRegex(RuntimeError, "debugfs"):
                FIRMWARE.extract_firmware(source, destination)
            self.assertFalse(destination.exists())
            source.write_bytes(b"not firmware")
            with patch.object(FIRMWARE.shutil, "which", return_value="/usr/sbin/debugfs"), \
                    patch.object(FIRMWARE.subprocess, "run") as run, self.assertRaises(ValueError):
                FIRMWARE.extract_firmware(source, destination)
            run.assert_not_called()
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()

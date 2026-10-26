#!/usr/bin/env python3
import argparse
from functools import lru_cache
import importlib.util
import hashlib
import lzma
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile

MAX_ROOTFS_BYTES = 512 * 1024 * 1024
MAX_GEN2_ROOTFS_BYTES = 1024 * 1024 * 1024
MAX_SECTION_TRAILER_BYTES = 64 * 1024 * 1024
MAX_XZ_MEMORY = 256 * 1024 * 1024
MAX_IMAGE_BYTES = 1024 * 1024 * 1024


def _image_identity(value: os.stat_result) -> tuple[int, int, int, int, int]:
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns


def _rootfs_region(source, prefix, size):
    if prefix[:4] == b"AZ01":
        offset = prefix.find(b"\xfd7zXZ\0")
        if offset < 0:
            raise ValueError("Expected an AZ01 image with an XZ root filesystem")
        return offset, size - offset
    if prefix[:4] != b"AZ0x" or len(prefix) < 56:
        raise ValueError("Unsupported firmware image format")
    version, reserved, header, strings, devices, first, end = struct.unpack_from("<7I", prefix, 4)
    device_count, section_count = struct.unpack_from("<HH", prefix, 36)
    if (version != 1 or reserved or header != 56 or strings != header
            or not strings < devices <= first < end <= size
            or first != devices + device_count * 8 or not 1 <= section_count <= 64
            or end != first + section_count * 64):
        raise ValueError("Invalid sectioned firmware header")
    source.seek(strings)
    names = source.read(devices - strings)
    previous, roots = end, []
    for position in range(first, end, 64):
        source.seek(position)
        record = source.read(64)
        if len(record) != 64:
            raise ValueError("Truncated firmware section table")
        kind, flags, offset, length, name, targets = struct.unpack_from("<4sIQQII", record)
        if (kind not in (b"BOOT", b"PARR", b"PART") or flags not in (1, 0x80000000)
                or offset < previous or not length or offset > size or length > size - offset
                or name >= len(names) or b"\0" not in names[name:]
                or (kind == b"BOOT" and (not targets or targets >> device_count))):
            raise ValueError("Invalid firmware section bounds")
        source.seek(offset)
        digest = hashlib.sha256()
        left = length
        while left:
            data = source.read(min(left, 1024 * 1024))
            if not data:
                raise ValueError("Firmware section is truncated")
            digest.update(data)
            left -= len(data)
        if digest.digest() != record[32:]:
            raise ValueError("Firmware section checksum differs")
        if kind == b"PART" and names[name:].split(b"\0", 1)[0] == b"rootfs":
            roots.append((offset, length))
        previous = offset + length
    if len(roots) != 1:
        raise ValueError("Expected exactly one firmware root filesystem section")
    return roots[0]


def extract_rootfs(image, destination):
    image, destination = Path(image), Path(destination)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Root filesystem destination already exists")
    created = False
    try:
        descriptor = os.open(image, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(descriptor, "rb") as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or not 12 <= before.st_size <= MAX_IMAGE_BYTES:
                raise ValueError("Firmware update image must be a regular file within the size limit")
            prefix = source.read(min(4096, before.st_size))
            offset, compressed_remaining = _rootfs_region(source, prefix, before.st_size)
            sectioned = prefix[:4] == b"AZ0x"
            maximum = MAX_GEN2_ROOTFS_BYTES if sectioned else MAX_ROOTFS_BYTES
            source.seek(offset)
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=MAX_XZ_MEMORY)
            total, expected = 0, None
            header, padding = bytearray(), bytearray()
            with destination.open("xb") as output:
                created = True
                while not decoder.eof:
                    compressed = source.read(min(65536, compressed_remaining)) if decoder.needs_input else b""
                    compressed_remaining -= len(compressed)
                    if decoder.needs_input and not compressed:
                        raise ValueError("Firmware root filesystem is truncated")
                    data = decoder.decompress(compressed, max_length=1024 * 1024)
                    total += len(data)
                    if total > maximum + (MAX_SECTION_TRAILER_BYTES if sectioned else 0):
                        raise ValueError("Firmware root filesystem size differs")
                    if expected is None:
                        header.extend(data)
                        if len(header) < 4096:
                            continue
                        if header[1080:1082] != b"\x53\xef":
                            raise ValueError("XZ stream is not an ext root filesystem")
                        blocks = struct.unpack_from("<I", header, 1028)[0]
                        shift = struct.unpack_from("<I", header, 1048)[0]
                        if shift > 4:
                            raise ValueError("Unsupported ext filesystem block size")
                        expected = blocks * (1024 << shift)
                        if not 4096 <= expected <= maximum:
                            raise ValueError("Firmware root filesystem exceeds fixture limits")
                        data = bytes(header)
                        header.clear()
                    if total > expected + (MAX_SECTION_TRAILER_BYTES if sectioned else 4096):
                        raise ValueError("Firmware root filesystem size differs")
                    if total > expected:
                        extra = min(len(data), total - expected)
                        if not sectioned:
                            padding.extend(data[-extra:])
                        data = data[:-extra]
                    output.write(data)
            if expected is None or total < expected or any(padding):
                raise ValueError("Firmware root filesystem length differs")
            try:
                unchanged = (_image_identity(os.fstat(source.fileno())) == _image_identity(before) and
                             _image_identity(image.stat()) == _image_identity(before))
            except OSError as error:
                raise ValueError("Firmware update image changed during extraction") from error
            if not unchanged:
                raise ValueError("Firmware update image changed during extraction")
    except BaseException:
        if created:
            destination.unlink(missing_ok=True)
        raise
    result = {"xz_offset": offset, "rootfs_bytes": expected, "stream_padding_bytes": len(padding)}
    if sectioned:
        result["section_trailer_bytes"] = total - expected
    return result


@lru_cache(maxsize=1)
def manager_package():
    spec = importlib.util.spec_from_file_location("manager_package", Path(__file__).resolve().parents[1] / "package.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Manager firmware validator is unavailable")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    return package


def validate_executable(executable):
    _, result = manager_package().validate_mpc_executable(executable)
    return result


def extract_firmware(image, destination, *, controller_helper=False):
    image = Path(image).resolve()
    requested = Path(destination)
    if requested.exists() or requested.is_symlink():
        raise FileExistsError("Extracted firmware destination already exists")
    root = requested.resolve()
    temporary = Path("/tmp").resolve()
    if root == temporary or not root.is_relative_to(temporary):
        raise ValueError("Extraction requires a nonexisting disposable directory under /tmp")
    if any(character in str(root) for character in '\0\n\r"\\'):
        raise ValueError("Extraction destination contains unsupported characters")
    if not image.is_file():
        raise ValueError("Update image must be an existing file")
    if shutil.which("debugfs") is None:
        raise RuntimeError("debugfs is required; install e2fsprogs")
    root.mkdir(mode=0o700)
    try:
        with tempfile.TemporaryDirectory(prefix="mpc-firmware-extract-") as directory:
            filesystem = Path(directory) / "rootfs.ext"
            result = extract_rootfs(image, filesystem)
            completed = subprocess.run(["debugfs", "-R", f'rdump / "{root}"', str(filesystem)],
                                       capture_output=True, timeout=180)
            if completed.returncode:
                raise RuntimeError("debugfs could not extract the firmware root filesystem")
        executable, busybox = root / "usr/bin/MPC", root / "usr/bin/busybox.nosuid"
        for file in (executable, busybox):
            if not file.is_file() or not file.resolve().is_relative_to(root):
                raise ValueError("Extracted firmware lacks the required MPC executable or busybox")
        if not busybox.stat().st_mode & 0o111:
            raise ValueError("Extracted busybox is not executable")
        if controller_helper:
            helper = manager_package()._helper()
            helper.profile_seed(executable)
            with executable.open("rb") as source:
                executable_hash = hashlib.file_digest(source, "sha256").hexdigest()
                source.seek(4)
                generation = "Gen2" if source.read(1) == b"\x02" else "Gen1"
            result.update(executable_sha256=executable_hash, controller_profile=generation + " OS 3.10")
        else:
            result.update(validate_executable(executable))
        result["firmware_executable_verified"] = True
        return result
    except BaseException:
        shutil.rmtree(root)
        raise


def main():
    parser = argparse.ArgumentParser(description="Extract a local MPC update with a compatible autorun scheme for offline packaging/tests")
    parser.add_argument("update_image", type=Path)
    parser.add_argument("destination", type=Path, help="Nonexisting disposable extracted root under /tmp")
    parser.add_argument("--controller-helper", action="store_true", help="Validate a supported OS 3.10 controller profile instead of legacy autorun")
    args = parser.parse_args()
    try:
        extract_firmware(args.update_image, args.destination, controller_helper=args.controller_helper)
    except (OSError, ValueError, RuntimeError, lzma.LZMAError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print("MPC firmware extracted; controller helper profile validated" if args.controller_helper
          else "MPC firmware extracted; executable structure and autorun salt validated")


if __name__ == "__main__":
    main()

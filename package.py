from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import struct
import zipfile
from pathlib import Path
from types import ModuleType

PROJECT = Path(__file__).resolve().parent
FIRMWARE_SHA256 = "bc054a3f3ba02c2d33ac9a515a4a8638964da779223502286d6a64b517bf1426"
EXPECTED_SALT_SHA256 = "5c1981d7d942710e1d0979c6f6c0b8d547d0d2c43283fc65bfa44bba3024e468"
MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024


def _scanner() -> ModuleType:
    spec = importlib.util.spec_from_file_location("mpc_saltscan", Path(__file__).resolve().parent / "tools/saltscan.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Firmware salt scanner is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _file_identity(value: os.stat_result) -> tuple[int, int, int, int, int]:
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns


def validate_mpc_executable(binary: Path) -> tuple[bytes, dict[str, object]]:
    binary = Path(binary)
    descriptor = os.open(binary, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        before = os.fstat(source.fileno())
        size = before.st_size
        if not stat.S_ISREG(before.st_mode) or not 52 <= size <= MAX_EXECUTABLE_BYTES:
            raise ValueError("MPC executable size or file type is unsupported")
        header = source.read(52)
        if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01" or header[7] not in (0, 3):
            raise ValueError("Expected an ELF32 little-endian ARM MPC executable")
        (kind, machine, version, entry, phoff, shoff, flags, ehsize,
         phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", header, 16)
        if kind not in (2, 3) or machine != 40 or version != 1 or ehsize != 52:
            raise ValueError("Expected an executable ELF32 ARM MPC binary")
        if flags & 0xff000000 != 0x05000000 or not flags & 0x400 or flags & 0x200:
            raise ValueError("MPC executable must use ARM EABI5 hard-float")
        if phentsize != 32 or not 1 <= phnum <= 1024 or phoff < ehsize or phoff + phnum * phentsize > size:
            raise ValueError("MPC executable has invalid program headers")
        if shnum:
            if shentsize != 40 or shoff < ehsize or shoff + shnum * shentsize > size or shstrndx >= shnum:
                raise ValueError("MPC executable has invalid section headers")
        elif shoff or shstrndx:
            raise ValueError("MPC executable has unsupported extended section headers")
        executable_entry, load_count, readable_ranges = False, 0, []
        source.seek(phoff)
        for _ in range(phnum):
            program = source.read(phentsize)
            if len(program) != phentsize:
                raise ValueError("MPC executable program headers are truncated")
            ptype, offset, address, _, filesz, memsz, permissions, alignment = struct.unpack("<8I", program)
            if offset > size or filesz > size - offset:
                raise ValueError("MPC executable segment exceeds file bounds")
            if ptype != 1:
                continue
            load_count += 1
            if filesz > memsz or address + memsz > 1 << 32 or permissions & ~7:
                raise ValueError("MPC executable has an invalid load segment")
            if alignment > 1 and (alignment & (alignment - 1) or offset % alignment != address % alignment):
                raise ValueError("MPC executable load alignment is invalid")
            if permissions & 4:
                readable_ranges.append((offset, offset + filesz))
            if permissions & 1 and permissions & 4 and address <= (entry & ~1) < address + filesz:
                executable_entry = True
        if not load_count or not executable_entry:
            raise ValueError("MPC executable entry is not in executable loaded content")
        source.seek(0)
        digest = hashlib.sha256()
        remaining = size
        while remaining:
            data = source.read(min(1024 * 1024, remaining))
            if not data:
                raise ValueError("MPC executable changed during validation")
            digest.update(data)
            remaining -= len(data)
        if _file_identity(os.fstat(source.fileno())) != _file_identity(before):
            raise ValueError("MPC executable changed during validation")
    scanner = _scanner()
    salt, locations = scanner.find_salt(binary, expected_sha256=EXPECTED_SALT_SHA256)
    if not any(where == "file" and any(first <= offset and offset + scanner.WIDTH <= last
                                       for first, last in readable_ranges)
               for where, offset in locations):
        raise ValueError("Autorun salt is not in readable loaded MPC content")
    if _file_identity(binary.stat()) != _file_identity(before):
        raise ValueError("MPC executable changed during salt scanning")
    checksum = digest.hexdigest()
    return salt, {
        "executable_sha256": checksum,
        "executable_bytes": size,
        "elf_type": "ET_EXEC" if kind == 2 else "ET_DYN",
        "autorun_salt_sha256": hashlib.sha256(salt).hexdigest(),
        "autorun_salt_locations": [[where, offset] for where, offset in locations],
        "matches_stock_gen1_reference": checksum == FIRMWARE_SHA256,
    }


def build_package(binary: Path, destination: Path, plugin: Path | None = None) -> Path:
    salt, _ = validate_mpc_executable(binary)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Destination already exists")
    if plugin is not None and not zipfile.is_zipfile(plugin):
        raise ValueError("Local package must be a ZIP file")
    required = [PROJECT / "build" / name for name in
                ("manager", "font.ttf", "shortcut", "shortcut-hook.so",
                 "font-license.txt", "json-license.txt")]
    required += [PROJECT / name for name in ("launch.sh", "setup.sh", "open.sh", "LICENSE")]
    for path in required:
        if not path.is_file():
            raise FileNotFoundError(f"Missing build input: {path.name}; run make arm first")
    if not (PROJECT / "licenses").is_dir():
        raise FileNotFoundError("Missing third-party licenses")

    autorun = destination / "autorun"
    runtime = autorun / "runtime"
    runtime.mkdir(parents=True)
    for name in ("manager", "font.ttf", "shortcut", "shortcut-hook.so"):
        shutil.copyfile(PROJECT / "build" / name, runtime / name)
    for name in ("launch.sh", "setup.sh"):
        shutil.copyfile(PROJECT / name, runtime / name)
    shutil.copyfile(PROJECT / "open.sh", autorun / "open.sh")
    licenses = runtime / "licenses"
    shutil.copytree(PROJECT / "licenses", licenses,
                    ignore=shutil.ignore_patterns("TitilliumWeb.txt"))
    shutil.copyfile(PROJECT / "LICENSE", licenses / "plugin-manager-usb.txt")
    for name in ("font-license.txt", "json-license.txt"):
        shutil.copyfile(PROJECT / "build" / name, licenses / name)
    local = destination / "local-plugins"
    local.mkdir()
    if plugin is not None:
        shutil.copyfile(plugin, local / plugin.name)
    (destination / ".mpc-manager-id").write_text("mpc-usb-manager-v1\n")
    files = sorted(str(path.relative_to(autorun)) for path in runtime.rglob("*") if path.is_file())
    value = {"value0": {"files": files, "scripts": [{
        "path": "open.sh", "askQuestion": True,
        "message": "MPC PLUGIN INSTALLER\n\nHave you saved your project?",
        "yesButtonText": "Open", "noButtonText": "Cancel",
    }]}}
    (autorun / "autorun.json").write_text(json.dumps(value, indent=2) + "\n")
    ordered = ["autorun.json", "open.sh"] + files
    checksum = hashlib.sha256(b"".join((autorun / name).read_bytes() for name in ordered) + salt)
    (autorun / "autorun.check").write_bytes(checksum.hexdigest().encode())
    manifest = {str(path.relative_to(destination)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(destination.rglob("*")) if path.is_file()}
    (destination / "USB-SHA256.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a manager-only USB bundle from local ARM outputs")
    parser.add_argument("firmware_executable", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("plugin", type=Path, nargs="?", help="Optional local plugin ZIP; omitted by default")
    args = parser.parse_args()
    try:
        build_package(args.firmware_executable, args.destination, args.plugin)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    print("USB Plugin Manager package generated")


if __name__ == "__main__":
    main()

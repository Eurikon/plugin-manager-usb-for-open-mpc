"""Build the controller helper from original source and local firmware inputs."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import zipfile
import zlib

PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / "src/startup"
INSTALLER = "Plugin Manager helper.xpa"
PREFIX = "Devices/Plugin Manager helper/"
RUNTIME = ("manager", "font.ttf", "launch.sh", "setup.sh", "shortcut", "shortcut-hook.so")
FIRMWARE_SHA256 = "753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494"
GEN2_FIRMWARE_SHA256 = "b80687ed4f6b5233486a9e20ff043b35e9863bc6460bd573a249421133d77b63"
PROFILE_SEED_SHA256 = "fc239913b495136422dbbed00f6350f64d65e7e0cd32dbb8e1253164b6131ed3"
OWNER = ("open-mpc-plugin-manager-test-v1\ncontroller.ini\nplugin_manager.lua\n"
         "PluginManager.xpj\nmanager-entry.so\nbootstrap.sh\nruntime.sha256\n"
         "manager-enabled\nsd-install-capability\ninstalled-storage\ninstalled-storage.removing\n").encode()
MEMBERS = {"PluginManager.xpj", "manager-entry.so", "bootstrap.sh", "runtime.sha256",
           ".owner", "plugin_manager.lua", "controller.ini", "manager-enabled", "sd-install-capability"}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def identity(value: os.stat_result) -> tuple[int, int, int, int, int]:
    return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns


def read_file(path: Path, maximum: int) -> bytes:
    path = Path(path)
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError("Unsupported helper input size or file type: " + path.name)
        data = source.read(maximum + 1)
        if (len(data) != before.st_size or identity(os.fstat(source.fileno())) != identity(before)
                or identity(path.stat()) != identity(before)):
            raise ValueError("Helper input changed while reading: " + path.name)
    return data


def profile_seed(path: Path) -> bytes:
    binary = read_file(path, 256 * 1024 * 1024)
    fingerprint = digest(binary)
    if fingerprint == FIRMWARE_SHA256:
        address = (0x521760 + struct.unpack_from("<I", binary, 0x521A00)[0]) & 0xFFFFFFFF
    elif fingerprint == GEN2_FIRMWARE_SHA256:
        address = 0x5287c10
    else:
        raise ValueError("The controller helper requires a tested stock MPC OS 3.10 executable")
    encoded = binary[address:address + 37]
    key = bytes.fromhex("4589c7d37349695f")
    decoded = bytes(value ^ key[index % 8] for index, value in enumerate(encoded))
    seed = decoded[:-1]
    if (len(decoded) != 37 or decoded[-1] != 0 or b"\0" in seed
            or digest(seed) != PROFILE_SEED_SHA256):
        raise ValueError("Unsupported controller checksum initialization")
    return seed


def setup_project(data: bytes) -> bytes:
    if not 0 < len(data) <= 1024 * 1024:
        raise ValueError("Setup project exceeds the compressed size limit")
    try:
        decoder = zlib.decompressobj(31)
        raw = decoder.decompress(data, 8 * 1024 * 1024 + 1)
        if len(raw) > 8 * 1024 * 1024 or not decoder.eof or decoder.unused_data:
            raise ValueError("Setup project is truncated, oversized or has trailing data")
        header = raw.split(b"\n", 5)
        if len(header) != 6 or header[0] != b"ACVS" or header[2:5] != [b"SerialisableProjectData", b"json", b"Linux"]:
            raise ValueError("Expected a standalone MPC project")
        project = json.loads(header[5])["data"]
        if not isinstance(project, dict) or project.get("samples") != [] or project.get("sequences") != []:
            raise ValueError("Save a blank setup project without samples or sequences")
    except (zlib.error, UnicodeError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError("Invalid blank MPC setup project") from error
    return data


def runtime_manifest(runtime: dict[str, bytes], arch: str = "armv7") -> bytes:
    if set(runtime) != set(RUNTIME):
        raise ValueError("Incomplete helper runtime")
    return "".join(digest(runtime[name]) + "  " + runtime_path(name, arch) + "\n" for name in RUNTIME).encode("ascii")


def runtime_path(name: str, arch: str) -> str:
    return "aarch64/" + name if arch == "aarch64" and name in ("manager", "shortcut", "shortcut-hook.so") else name


def runtime_files(arch: str = "armv7") -> dict[str, bytes]:
    return {name: read_file(PROJECT / (name if name in ("launch.sh", "setup.sh") else "build/" + runtime_path(name, arch)),
                            64 * 1024 * 1024) for name in RUNTIME}


def validate_archive(data: bytes, runtime: dict[str, bytes], runtime64: dict[str, bytes] | None = None) -> dict[str, bytes]:
    members = MEMBERS | ({"manager-entry-aarch64.so", "runtime-aarch64.sha256"} if runtime64 is not None else set())
    if not 0 < len(data) <= 2 * 1024 * 1024:
        raise ValueError("Helper archive exceeds its size limit")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            if (len(entries) != len(members) or {x.filename for x in entries} != {PREFIX + n for n in members}
                    or any(x.file_size > 1024 * 1024 or x.flag_bits & 1
                           or stat.S_IFMT(x.external_attr >> 16) not in (0, stat.S_IFREG) for x in entries)
                    or archive.testzip() is not None):
                raise ValueError("Invalid controller helper archive members")
            files = {name: archive.read(PREFIX + name) for name in members}
    except (zipfile.BadZipFile, RuntimeError) as error:
        raise ValueError("Damaged controller helper archive") from error
    if (files["runtime.sha256"] != runtime_manifest(runtime) or files[".owner"] != (OWNER + b"manager-entry-aarch64.so\nruntime-aarch64.sha256\n" if runtime64 is not None else OWNER)
            or files["manager-enabled"] != b"open-mpc-manager-reusable-v1\n"
            or files["sd-install-capability"] != b"open-mpc-manager-sd-v1\n"):
        raise ValueError("Helper identity, activation or runtime binding mismatch")
    project = setup_project(files["PluginManager.xpj"])
    bootstrap = files["bootstrap.sh"].decode("utf-8")
    if (re.findall(r"^PROJECT_BYTES=(\d+)$", bootstrap, re.M) != [str(len(project))]
            or re.findall(r"^PROJECT_SHA256='([0-9a-f]{64})'$", bootstrap, re.M) != [digest(project)]):
        raise ValueError("Setup project cleanup binding mismatch")
    module = files["manager-entry.so"]
    if len(module) < 52 or module[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", module, 18)[0] != 40:
        raise ValueError("Expected an ARM32 helper module")
    if runtime64 is not None:
        module64 = files["manager-entry-aarch64.so"]
        if (files["runtime-aarch64.sha256"] != runtime_manifest(runtime64, "aarch64")
                or len(module64) < 64 or module64[:7] != b"\x7fELF\x02\x01\x01"
                or struct.unpack_from("<H", module64, 18)[0] != 183):
            raise ValueError("Invalid ARM64 helper runtime or module")
    return files


def build_archive(firmware: Path, project: Path, firmware64: Path | None = None) -> bytes:
    seed = profile_seed(firmware)
    if firmware64 is not None and profile_seed(firmware64) != seed:
        raise ValueError("Controller checksum initialization differs between generations")
    project_data = setup_project(read_file(project, 1024 * 1024))
    lua = read_file(SOURCE / "plugin_manager.lua", 64 * 1024)
    if lua.count(b"local ENABLED = false\n") != 1:
        raise ValueError("Unexpected controller script activation template")
    lua = lua.replace(b"local ENABLED = false\n", b"local ENABLED = true\n", 1)
    bootstrap = read_file(SOURCE / "bootstrap.sh", 64 * 1024).decode("utf-8")
    for pattern, replacement in ((r"^PROJECT_BYTES=\d+$", "PROJECT_BYTES=" + str(len(project_data))),
                                 (r"^PROJECT_SHA256='[0-9a-f]{64}'$", "PROJECT_SHA256='" + digest(project_data) + "'")):
        bootstrap, count = re.subn(pattern, replacement, bootstrap, flags=re.M)
        if count != 1:
            raise ValueError("Missing setup-project cleanup template")
    runtime = runtime_files()
    files = {"PluginManager.xpj": project_data,
             "manager-entry.so": read_file(PROJECT / "build/startup/manager-entry.so", 1024 * 1024),
             "bootstrap.sh": bootstrap.encode(), "runtime.sha256": runtime_manifest(runtime),
             ".owner": OWNER, "plugin_manager.lua": lua,
             "controller.ini": ("[info]\ndevice=Plugin Manager helper\nscript=plugin_manager.lua\n"
                                "scriptVersion=1.0\nchecksum=" + digest(lua + seed) + "\n").encode(),
             "manager-enabled": b"open-mpc-manager-reusable-v1\n",
             "sd-install-capability": b"open-mpc-manager-sd-v1\n"}
    runtime64 = runtime_files("aarch64") if firmware64 is not None else None
    if runtime64 is not None:
        files[".owner"] += b"manager-entry-aarch64.so\nruntime-aarch64.sha256\n"
        files["manager-entry-aarch64.so"] = read_file(PROJECT / "build/aarch64/startup/manager-entry.so", 1024 * 1024)
        files["runtime-aarch64.sha256"] = runtime_manifest(runtime64, "aarch64")
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name in ["PluginManager.xpj", *sorted(set(files) - {"PluginManager.xpj"})]:
            entry = zipfile.ZipInfo(PREFIX + name)
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | 0o600) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, files[name])
    data = output.getvalue()
    validate_archive(data, runtime, runtime64)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description="Package the MPC OS 3.10 controller helper")
    parser.add_argument("firmware_executable", type=Path)
    parser.add_argument("setup_project", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        data = build_archive(args.firmware_executable, args.setup_project)
        with args.destination.open("xb") as output:
            output.write(data)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print("Controller helper generated: " + str(args.destination))


if __name__ == "__main__":
    main()

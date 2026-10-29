from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import struct
import tempfile
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


def _helper() -> ModuleType:
    spec = importlib.util.spec_from_file_location("mpc_helper_package", PROJECT / "tools/helper_package.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Controller helper packager is unavailable")
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
        header = source.read(64)
        if len(header) < 52 or header[:4] != b"\x7fELF" or header[5:7] != b"\x01\x01" or header[7] not in (0, 3):
            raise ValueError("Expected a little-endian ARM MPC executable")
        is64 = header[4] == 2
        if header[4] not in (1, 2) or (is64 and len(header) != 64):
            raise ValueError("Unsupported MPC ELF class")
        format_ = "<HHIQQQIHHHHHH" if is64 else "<HHIIIIIHHHHHH"
        (kind, machine, version, entry, phoff, shoff, flags, ehsize,
         phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from(format_, header, 16)
        if (kind not in (2, 3) or machine != (183 if is64 else 40) or version != 1
                or ehsize != (64 if is64 else 52)):
            raise ValueError("Expected an executable ARM MPC binary")
        if (is64 and flags != 0) or (not is64 and (flags & 0xff000000 != 0x05000000 or not flags & 0x400 or flags & 0x200)):
            raise ValueError("MPC executable must use the supported ARM ABI")
        if phentsize != (56 if is64 else 32) or not 1 <= phnum <= 1024 or phoff < ehsize or phoff + phnum * phentsize > size:
            raise ValueError("MPC executable has invalid program headers")
        if shnum:
            if shentsize != (64 if is64 else 40) or shoff < ehsize or shoff + shnum * shentsize > size or shstrndx >= shnum:
                raise ValueError("MPC executable has invalid section headers")
        elif shoff or shstrndx:
            raise ValueError("MPC executable has unsupported extended section headers")
        executable_entry, load_count, readable_ranges = False, 0, []
        source.seek(phoff)
        for _ in range(phnum):
            program = source.read(phentsize)
            if len(program) != phentsize:
                raise ValueError("MPC executable program headers are truncated")
            if is64:
                ptype, permissions, offset, address, _, filesz, memsz, alignment = struct.unpack("<II6Q", program)
            else:
                ptype, offset, address, _, filesz, memsz, permissions, alignment = struct.unpack("<8I", program)
            if offset > size or filesz > size - offset:
                raise ValueError("MPC executable segment exceeds file bounds")
            if ptype != 1:
                continue
            load_count += 1
            if filesz > memsz or address + memsz > 1 << (64 if is64 else 32) or permissions & ~7:
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
        "arch": "aarch64" if is64 else "armv7",
        "executable_bytes": size,
        "elf_type": "ET_EXEC" if kind == 2 else "ET_DYN",
        "autorun_salt_sha256": hashlib.sha256(salt).hexdigest(),
        "autorun_salt_locations": [[where, offset] for where, offset in locations],
        "matches_stock_gen1_reference": checksum == FIRMWARE_SHA256,
    }


def build_package(binary: Path, destination: Path, plugin: Path | None = None,
                  *, helper: bytes | None = None, arm64_build: Path | None = None) -> Path:
    salt, facts = validate_mpc_executable(binary)
    if facts.get("arch") == "aarch64" and arm64_build is None:
        raise ValueError("A Gen 2 firmware input requires ARM64 runtime outputs")
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

    if arm64_build is not None:
        arm64_build = Path(arm64_build)
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            path = arm64_build / name
            with path.open("rb") as source:
                header = source.read(64)
            if (len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01"
                    or struct.unpack_from("<H", header, 18)[0] != 183):
                raise ValueError("Expected an ARM64 build input: " + name)
    autorun = destination / "autorun"
    runtime = autorun / "runtime"
    runtime.mkdir(parents=True)
    for name in ("manager", "font.ttf", "shortcut", "shortcut-hook.so"):
        shutil.copyfile(PROJECT / "build" / name, runtime / name)
    if arm64_build is not None:
        (runtime / "aarch64").mkdir()
        for name in ("manager", "shortcut", "shortcut-hook.so"):
            shutil.copyfile(arm64_build / name, runtime / "aarch64" / name)
    for name in ("launch.sh", "setup.sh"):
        shutil.copyfile(PROJECT / name, runtime / name)
    if helper is not None:
        packager = _helper()
        packager.validate_archive(helper, {name: (runtime / name).read_bytes() for name in packager.RUNTIME},
                                  {name: (runtime / packager.runtime_path(name, "aarch64")).read_bytes()
                                   for name in packager.RUNTIME} if arm64_build is not None else None)
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
    if helper is not None:
        (destination / packager.INSTALLER).write_bytes(helper)
        manifest[packager.INSTALLER] = hashlib.sha256(helper).hexdigest()
    (destination / "USB-SHA256.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return destination


def build_universal_package(binary: Path, firmware_310: Path, setup_project: Path,
                            destination: Path, plugin: Path | None = None, *, arm64_build: Path | None = None, firmware_310_arm64: Path | None = None) -> Path:
    destination = Path(destination)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Destination already exists")
    if (arm64_build is None) != (firmware_310_arm64 is None):
        raise ValueError("Supply both ARM64 outputs and the Gen 2 OS 3.10 executable")
    helper = _helper().build_archive(firmware_310, setup_project, firmware_310_arm64) if firmware_310_arm64 is not None else _helper().build_archive(firmware_310, setup_project)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".manager-package-", dir=destination.parent) as temporary:
        staged = Path(temporary) / "usb"
        build_package(binary, staged, plugin, helper=helper, arm64_build=arm64_build)
        if destination.exists() or destination.is_symlink():
            raise FileExistsError("Destination appeared during packaging")
        staged.rename(destination)
    return destination


def build_signed_package(binary: Path, firmware_310: Path, setup_project: Path,
                         destination: Path, signing_key: Path, *, arm64_build: Path | None = None,
                         firmware_310_arm64: Path | None = None) -> Path:
    from tools.payload_package import encode, envelope, generation, store
    from tools.sign_release import Signer
    import io
    import re
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Destination already exists")
    if (arm64_build is None) != (firmware_310_arm64 is None):
        raise ValueError("Supply both ARM64 outputs and the Gen 2 OS 3.10 executable")
    salt, facts = validate_mpc_executable(binary)
    if facts["arch"] != "armv7":
        raise ValueError("The universal stick uses the stock Gen 1 autorun format")
    match = re.search(r'^#define MPC_MANAGER_VERSION "([A-Za-z0-9_.-]+)"$',
                      (PROJECT / "src/version.hpp").read_text(), re.M)
    if match is None:
        raise ValueError("Missing manager release version")
    version = match.group(1)
    signer = Signer(signing_key, PROJECT / "src/publisher_key.hpp")
    packager = _helper()
    helper = packager.build_versioned_archive(firmware_310, setup_project, signer, version, firmware_310_arm64)
    payload = {"font.ttf": packager.read_file(PROJECT / "build/font.ttf", 4 * 1024 * 1024),
               "launch.sh": (PROJECT / "launch.sh").read_bytes(), "setup.sh": (PROJECT / "setup.sh").read_bytes(),
               "resources/Plugin Manager helper.xpa": helper}
    roles = {"font": "font.ttf", "launch": "launch.sh", "setup": "setup.sh",
             "helper_archive": "resources/Plugin Manager helper.xpa"}
    files = {".mpc-manager-id": b"mpc-usb-manager-v1\n", "Plugin Manager helper.xpa": helper,
             "autorun/open.sh": (PROJECT / "open.sh").read_bytes()}
    architectures = ("armv7", "aarch64") if arm64_build is not None else ("armv7",)
    for arch in architectures:
        build = arm64_build if arch == "aarch64" else PROJECT / "build"
        for name, role in (("manager", "manager"), ("shortcut", "supervisor"), ("shortcut-hook.so", "hook")):
            relative = "bin/" + arch + "/" + name
            payload[relative] = packager.native_file(build / name, arch, 64 * 1024 * 1024)
            roles[role + "_" + arch] = relative
        roles["runtime_worker_" + arch] = roles["manager_" + arch]
        files["autorun/runtime/source-dispatcher-" + arch] = packager.native_file(build / "source-dispatcher", arch, 2 * 1024 * 1024)
    for path in sorted((PROJECT / "licenses").glob("*")):
        if path.is_file() and path.name != "TitilliumWeb.txt":
            payload["licenses/" + path.name] = path.read_bytes()
    payload["licenses/plugin-manager-usb.txt"] = (PROJECT / "LICENSE").read_bytes()
    for name in ("font-license.txt", "json-license.txt"):
        payload["licenses/" + name] = (PROJECT / "build" / name).read_bytes()
    architecture = "universal" if len(architectures) == 2 else "armv7"
    identifier, _, signed = generation(payload, roles, version, architecture, "runtime", signer)
    files.update({"autorun/runtime/" + name: data for name, data in store(identifier, signed, architectures).items()})
    helper_payload = {}
    with zipfile.ZipFile(io.BytesIO(helper)) as archive:
        for name in archive.namelist():
            relative = name.removeprefix(packager.PREFIX)
            if relative.startswith(".pmu-versions/") and len(relative.split("/")) >= 3:
                helper_payload[relative.split("/", 2)[2]] = archive.read(name)
    if "generation.json" not in helper_payload or "authorization.json" not in helper_payload:
        raise ValueError("Helper has no complete signed generation")
    files.update({"updates/helper/" + name: data for name, data in helper_payload.items()})
    runtime_directory = "autorun/runtime/.pmu-versions/" + identifier
    recipe = {"schema": 1, "product": "plugin-manager-usb",
              "runtime": {"directory": runtime_directory, "manifest": runtime_directory + "/generation.json",
                          "approval": runtime_directory + "/authorization.json"},
              "helper": {"directory": "updates/helper", "manifest": "updates/helper/generation.json",
                         "approval": "updates/helper/authorization.json"}}
    files["updates/deployment.json"] = encode(recipe)
    declared = sorted("runtime/source-dispatcher-" + arch for arch in architectures)
    files["autorun/autorun.json"] = (json.dumps({"value0": {"files": declared, "scripts": [{
        "path": "open.sh", "askQuestion": True,
        "message": "MPC PLUGIN INSTALLER\n\nHave you saved your project?",
        "yesButtonText": "Open", "noButtonText": "Cancel"}]}}, indent=2) + "\n").encode()
    checked = ["autorun.json", "open.sh", *declared]
    files["autorun/autorun.check"] = hashlib.sha256(b"".join(files["autorun/" + name] for name in checked) + salt).hexdigest().encode()
    files["USB-SHA256.json"] = encode({name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
    outer_roles = {"deployment": "updates/deployment.json"}
    for arch in architectures:
        outer_roles["update_worker_" + arch] = runtime_directory + "/" + roles["runtime_worker_" + arch]
    files["PMU-UPDATE.json"] = envelope(files, outer_roles, version, architecture, signer)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".manager-signed-", dir=destination.parent) as temporary:
        staged = Path(temporary) / "bundle"
        staged.mkdir()
        for name, data in files.items():
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (staged / "local-plugins").mkdir()
        if destination.exists() or destination.is_symlink():
            raise FileExistsError("Destination appeared during signing")
        staged.rename(destination)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a legacy or universal manager USB bundle from local ARM outputs")
    parser.add_argument("firmware_executable", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("plugin", type=Path, nargs="?", help="Optional local plugin ZIP; omitted by default")
    parser.add_argument("--mpc-310-arm64", type=Path, help="Stock Gen 2 OS 3.10 executable for the universal helper")
    parser.add_argument("--arm64-build", type=Path, help="Additional ARM64 runtime build directory for Gen 2")
    parser.add_argument("--mpc-310", type=Path, help="Stock Gen1 OS 3.10 MPC executable for the controller helper")
    parser.add_argument("--setup-project", type=Path, help="Blank standalone MPC project for the one-time helper import")
    parser.add_argument("--signing-key", type=Path, help="Private Ed25519 key matching the compiled publisher header")
    parser.add_argument("--legacy-layout", action="store_true", help="Explicitly build the older unsigned layout for compatibility testing")
    args = parser.parse_args()
    if bool(args.signing_key) == args.legacy_layout:
        parser.error("Supply --signing-key for a signed bundle, or --legacy-layout for compatibility testing")
    if args.mpc_310_arm64 is not None and args.mpc_310 is None:
        parser.error("--mpc-310-arm64 requires --mpc-310 and --setup-project")
    if (args.mpc_310 is None) != (args.setup_project is None):
        parser.error("Supply both --mpc-310 and --setup-project for a universal bundle")
    try:
        if args.signing_key is not None:
            if args.mpc_310 is None or args.setup_project is None or args.plugin is not None:
                parser.error("Signed distribution needs --mpc-310, --setup-project and no bundled plugin")
            build_signed_package(args.firmware_executable, args.mpc_310, args.setup_project, args.destination, args.signing_key, arm64_build=args.arm64_build, firmware_310_arm64=args.mpc_310_arm64)
        elif args.mpc_310 is not None:
            build_universal_package(args.firmware_executable, args.mpc_310, args.setup_project,
                                    args.destination, args.plugin, arm64_build=args.arm64_build, firmware_310_arm64=args.mpc_310_arm64)
        else:
            build_package(args.firmware_executable, args.destination, args.plugin, arm64_build=args.arm64_build)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    print("Plugin Manager USB package generated")


if __name__ == "__main__":
    main()

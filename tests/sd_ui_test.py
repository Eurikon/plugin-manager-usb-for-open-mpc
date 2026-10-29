import argparse
import binascii
import hashlib
import io
import json
import re
from pathlib import Path
import subprocess
import struct
import time
import uuid
import zlib
import zipfile


SOURCE = Path(__file__).resolve().parents[1]
MODES = ("sd-unavailable", "sd-empty", "sd-cancel", "sd-install", "sd-remove", "sd-remove-current",
         "sd-hidden", "sd-themes", "sd-updates", "sd-update-error", "sd-self-update", "sd-self-update-fail")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compact_capture(path):
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    if (data[:2] != b"BM" or (width, abs(height)) != (1280, 800)
            or struct.unpack_from("<H", data, 28)[0] != 32
            or struct.unpack_from("<4I", data, 54) != (0xff0000, 0xff00, 0xff, 0xff000000)
            or len(data) != offset + width * abs(height) * 4):
        raise ValueError("Unexpected SDL capture format")
    rows = []
    for y in (range(height - 1, -1, -1) if height > 0 else range(-height)):
        source = data[offset + y * width * 4:offset + (y + 1) * width * 4]
        row = bytearray(len(source))
        row[0::4], row[1::4], row[2::4], row[3::4] = source[2::4], source[1::4], source[0::4], source[3::4]
        rows.append(b"\0" + row)
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", binascii.crc32(kind + value) & 0xffffffff)
    output = path.with_suffix(".png")
    output.write_bytes(b"\x89PNG\r\n\x1a\n"
                      + chunk(b"IHDR", struct.pack(">IIBBBBB", width, abs(height), 8, 6, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
    path.unlink()
    return output



def next_version():
    match = re.search(r'^#define MPC_MANAGER_VERSION "(\d+)\.(\d+)\.(\d+)"$',
                      (SOURCE / "src/version.hpp").read_text(), re.MULTILINE)
    if match is None:
        raise ValueError("Missing manager release version")
    major, minor, patch = map(int, match.groups())
    return f"{major}.{minor}.{patch + 1}"


def update_fixture(root):
    version = next_version()
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", header, 16, 2, 40)
    struct.pack_into("<H", header, 40, 52)
    runtime = {name: bytes(header) + name.encode() for name in ("manager", "shortcut", "shortcut-hook.so")}
    runtime.update({"font.ttf": b"fixture font", "launch.sh": b"#!/bin/sh\nexit 0\n", "setup.sh": b"#!/bin/sh\nexit 0\n"})
    files = {"autorun/runtime/" + name: value for name, value in runtime.items()}
    for name in ("plugin-manager-usb.txt", "SDL2.txt", "community-tools.txt", "font-license.txt", "json-license.txt", "manager.txt"):
        files["autorun/runtime/licenses/" + name] = b"Fixture license\n"
    binding = "".join(hashlib.sha256(runtime[name]).hexdigest() + "  " + name + "\n"
                      for name in ("manager", "font.ttf", "launch.sh", "setup.sh", "shortcut", "shortcut-hook.so"))
    helper = {name: (name + " fixture\n").encode() for name in (
        ".owner", "PluginManager.xpj", "bootstrap.sh", "controller.ini", "manager-entry.so", "plugin_manager.lua")}
    helper.update({"manager-enabled": b"open-mpc-manager-reusable-v1\n",
                   "sd-install-capability": b"open-mpc-manager-sd-v1\n", "runtime.sha256": binding.encode()})
    def archive(entries):
        content = io.BytesIO()
        with zipfile.ZipFile(content, "w", zipfile.ZIP_DEFLATED) as output:
            for name, value in entries.items():
                entry = zipfile.ZipInfo(name)
                entry.external_attr = 0o100644 << 16
                output.writestr(entry, value)
        return content.getvalue()
    files["Plugin Manager helper.xpa"] = archive({"Devices/Plugin Manager helper/" + name: value for name, value in helper.items()})
    files.update({".mpc-manager-id": b"mpc-usb-manager-v1\n", "autorun/open.sh": b"#!/bin/sh\nexit 0\n",
                  "autorun/autorun.check": b"0" * 64,
                  "autorun/autorun.json": json.dumps({"value0": {"files": sorted(name[8:] for name in files if name.startswith("autorun/runtime/")), "scripts": [{"path": "open.sh"}]}}).encode()})
    manifest = {name: hashlib.sha256(value).hexdigest() for name, value in files.items()}
    files["USB-SHA256.json"] = json.dumps(manifest).encode()
    package = root / "update.zip"
    package.write_bytes(archive(files))
    release = {"draft": False, "prerelease": False, "tag_name": "v" + version,
               "html_url": f"https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v{version}",
               "assets": [{"name": f"plugin-manager-usb-v{version}.zip", "state": "uploaded",
                           "size": package.stat().st_size, "digest": "sha256:" + digest(package),
                           "browser_download_url": f"https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/download/v{version}/plugin-manager-usb-v{version}.zip"}]}
    (root / "release.json").write_text(json.dumps(release))
    for name, value in {"manager-preferences.json": json.dumps({"schema": 1, "dark_theme": False, "hide_welcome": False}),
                        "plugin-history.json": json.dumps({"schema": 1, "plugins": {}}), "local-plugins/keep.txt": "User package fixture\n"}.items():
        path = root / "source" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
    return {name: digest(root / "source" / name) for name in ("manager-preferences.json", "plugin-history.json", "local-plugins/keep.txt")}


def main():
    parser = argparse.ArgumentParser(description="Check the SD setup interface using isolated storage fixtures")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="mpc-usb-plugin-installer-build-tools")
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(SOURCE / "build"):
        parser.error("Use a fresh output directory inside build/")
    inputs = [SOURCE / name for name in (
        "src/main.cpp", "src/sd_install.cpp", "src/sd_install.hpp", "tests/sd_install_fixture.hpp",
        "src/backend.cpp", "src/plugin_storage.cpp", "src/plugin_storage.hpp", "src/backend.hpp", "src/version.hpp", "src/manager_update.cpp", "src/manager_update.hpp", "tests/manager_update_fixture.hpp",
        "src/thumbnail.hpp", "src/thumbnail.cpp", "build/thumbnail.o", "build/backend.o", "build/plugin_storage.o", "build/sd_install.o", "build/manager_update.o", "build/manager", "build/font.ttf")]
    hashes = {str(path.relative_to(SOURCE)): digest(path) for path in inputs}
    output.mkdir(parents=True, exist_ok=False)
    preserved = {}
    version = next_version()
    for mode in (*MODES, "sd-install-dark", "legacy-smoke"):
        root = output / mode
        for folder in ("source", "session", "Expansions", "bin"):
            (root / folder).mkdir(parents=True)
        (root / "source/.mpc-manager-id").write_text("mpc-usb-manager-v1\n")
        (root / "MPC.settings").write_text("<PROPERTIES/>\n")
        if mode in ("sd-install-dark", "sd-hidden"):
            (root / "source/manager-preferences.json").write_text(json.dumps({
                "schema": 1, "dark_theme": mode == "sd-install-dark", "hide_welcome": mode == "sd-hidden"}))
        downloader = root / "bin/wget"
        if mode in ("sd-self-update", "sd-self-update-fail"):
            preserved[mode] = update_fixture(root)
            downloader.write_text("""#!/bin/sh
root=${0%/bin/wget}
out=
url=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -O) out=$2; shift 2;;
    https://*) url=$1; shift;;
    *) shift;;
  esac
done
[ -n "$out" ] || exit 2
case "$url" in
  https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest)
    printf 'check\\n' >> "$root/session/manager-checks.log"
    cp "$root/release.json" "$out";;
  https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/download/v@VERSION@/plugin-manager-usb-v@VERSION@.zip)
    printf 'download\\n' >> "$root/session/manager-downloads.log"
    sleep 1
    cp "$root/update.zip" "$out";;
  *) exit 1;;
esac
""")
        elif mode in ("sd-updates", "sd-update-error"):
            downloader.write_text("""#!/bin/sh
out=
while [ "$#" -gt 0 ]; do
  case "$1" in
    -O) out=$2; shift 2;;
    https://api.github.com/repos/Eurikon/plugin-manager-usb-for-open-mpc/releases/latest) break;;
    *) shift;;
  esac
done
[ "$#" -gt 0 ] || exit 1
[ -n "$out" ] || exit 2
printf 'check\\n' >> "${out%/*}/manager-checks.log"
sleep 1
""" + ("exit 1\n" if mode == "sd-update-error" else """cat > "$out" <<'JSON'
{"draft":false,"prerelease":false,"tag_name":"v@VERSION@","html_url":"https://github.com/Eurikon/plugin-manager-usb-for-open-mpc/releases/tag/v@VERSION@"}
JSON
"""))
        else:
            downloader.write_text("#!/bin/sh\nexit 1\n")
        downloader.write_text(downloader.read_text().replace("@VERSION@", version))
        downloader.chmod(0o755)
    script = """set -eu
c++ -std=c++17 -O2 -Wall -Wextra -Werror -DMPC_MANAGER_SD_FIXTURE -Isrc -Itests -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o /results/main.o
c++ /results/main.o build/backend.o build/plugin_storage.o build/sd_install.o build/manager_update.o build/thumbnail.o -o /results/manager-ui $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive -Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic
for mode in sd-unavailable sd-empty sd-cancel sd-install sd-remove sd-remove-current sd-hidden sd-themes sd-updates sd-update-error sd-self-update sd-self-update-fail sd-install-dark legacy-smoke; do
  root=/results/$mode
  binary=/results/manager-ui
  testmode=$mode
  if [ "$mode" = legacy-smoke ]; then binary=/work/build/manager; testmode=smoke; fi
  if [ "$mode" = sd-install-dark ]; then testmode=sd-install; fi
  printf 'MODE %s\\n' "$mode"
  env -u MPC_MANAGER_SD_HELPER PATH="$root/bin:$PATH" SDL_VIDEODRIVER=dummy MPC_MANAGER_UI_UPDATE_VERSION="@VERSION@" MPC_MANAGER_UI_TEST="$testmode" "$binary" "$root/source" "$root/session" "$root/Expansions" "$root/MPC.settings" /work/build/font.ttf
done
"""
    script = script.replace("@VERSION@", version)
    container = "mpc-manager-sd-ui-" + uuid.uuid4().hex[:12]
    command = ["docker", "run", "--rm", "--pull=never", "--name", container,
               "--platform=linux/arm/v7", "--network=none", "--read-only", "--memory=768m",
               "--pids-limit=128", "--tmpfs=/tmp:rw,exec,size=67108864",
               "--mount", f"type=bind,src={SOURCE},dst=/work,readonly",
               "--mount", f"type=bind,src={output},dst=/results", "--workdir=/work",
               args.image, "sh", "-ec", script]
    report = {"date": time.strftime("%d-%m-%Y"), "source_sha256": hashes,
              "scope": "ARM manager interface; isolated storage identities and unavailable network; no device operations"}
    try:
        with (output / "tests.log").open("x") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=240)
        report["returncode"] = result.returncode
    except subprocess.TimeoutExpired:
        report.update(returncode=124, error="UI fixture timeout")
    finally:
        subprocess.run(["docker", "rm", "-f", container], capture_output=True, timeout=15)
        gone = subprocess.run(["docker", "container", "inspect", container], capture_output=True, timeout=15)
        report["container_removed"] = gone.returncode == 1 and b"No such container" in gone.stderr
        report["sources_unchanged"] = all(digest(SOURCE / name) == value for name, value in hashes.items())
    text = (output / "tests.log").read_text()
    report["checks"] = [line[6:] for line in text.splitlines() if line.startswith("PASS: ")]
    report["single_update_request"] = all(
        (output / mode / "session/manager-checks.log").is_file()
        and (output / mode / "session/manager-checks.log").read_text().splitlines() == ["check"]
        for mode in ("sd-updates", "sd-update-error"))
    report["hidden_preference_restored"] = json.loads(
        (output / "sd-hidden/source/manager-preferences.json").read_text()).get("hide_welcome") is False
    report["release_temp_removed"] = all(
        not (output / mode / "session/manager-release.json").exists()
        for mode in ("sd-updates", "sd-update-error"))
    report["self_update"] = {}
    for mode, original in preserved.items():
        root = output / mode
        manifest_path = root / "source/USB-SHA256.json"
        installed = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
        with zipfile.ZipFile(root / "update.zip") as archive:
            released = json.loads(archive.read("USB-SHA256.json"))
        passed = (root / "session/manager-checks.log").is_file() and (root / "session/manager-downloads.log").is_file()
        if passed:
            passed = ((root / "session/manager-checks.log").read_text().splitlines() == ["check", "check"]
                      and (root / "session/manager-downloads.log").read_text().splitlines() == ["download"])
        staging = list((root / "session").glob("manager-update.*/bundle"))
        report["self_update"][mode] = bool(
            passed and installed and all(digest(root / "source" / name) == value for name, value in installed.items())
            and all(digest(root / "source" / name) == value for name, value in original.items())
            and ((installed == released and not staging) if mode == "sd-self-update"
                 else (installed != released and len(staging) == 1)))
    report["captures"] = [str(compact_capture(path).relative_to(output)) for path in sorted(output.rglob("*.bmp"))]
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"returncode": report["returncode"], "checks": len(report["checks"]), "report": str(output / "results.json")}))
    return 0 if report["returncode"] == 0 and report["sources_unchanged"] and report["container_removed"] and report["single_update_request"] and report["hidden_preference_restored"] and report["release_temp_removed"] and all(report["self_update"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())

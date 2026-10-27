import argparse
import json
from pathlib import Path
import subprocess
import time
import uuid

from sd_ui_test import compact_capture


SOURCE = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description="Check Gen2 plugin-location selection without a device")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(SOURCE / "build"):
        parser.error("Use a fresh output directory inside build/")
    output.mkdir(parents=True, exist_ok=False)
    modes = ("plugin-location", "plugin-location-blocked", "plugin-location-custom", "plugin-location-dark")
    for mode in modes:
        root = output / mode
        for name in ("source", "session", "bin"):
            (root / name).mkdir(parents=True)
        (root / "source/.mpc-manager-id").write_text("mpc-usb-manager-v1\n")
        if mode == "plugin-location-dark":
            (root / "source/manager-preferences.json").write_text(json.dumps({"schema": 1, "dark_theme": True, "hide_welcome": False}))
        downloader = root / "bin/wget"
        downloader.write_text("#!/bin/sh\nexit 1\n")
        downloader.chmod(0o755)
    script = """set -eu
c++ -std=c++17 -O2 -Wall -Wextra -Werror -DMPC_MANAGER_PLUGIN_STORAGE_FIXTURE -Isrc -Itests -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o /results/main.o
c++ /results/main.o build/aarch64/backend.o build/aarch64/plugin_storage.o build/aarch64/sd_install.o build/aarch64/manager_update.o -o /results/manager-ui $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive
for mode in plugin-location plugin-location-blocked plugin-location-custom plugin-location-dark; do
  root=/results/$mode
  PATH="$root/bin:$PATH" SDL_VIDEODRIVER=dummy MPC_MANAGER_UI_TEST="$mode" /results/manager-ui "$root/source" "$root/session" auto "$root/MPC.settings" /work/build/aarch64/font.ttf
done
"""
    container = "pmu-plugin-storage-ui-" + uuid.uuid4().hex[:12]
    command = ["docker", "run", "--rm", "--pull=never", "--name", container, "--platform=linux/arm64", "--network=none",
               "--read-only", "--memory=768m", "--pids-limit=128", "--tmpfs=/tmp:rw,exec,size=67108864",
               "--mount", f"type=bind,src={SOURCE},dst=/work,readonly",
               "--mount", f"type=bind,src={output},dst=/results", "--workdir=/work",
               "mpc-usb-plugin-installer-build-tools:aarch64", "sh", "-ec", script]
    try:
        with (output / "tests.log").open("x") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    finally:
        subprocess.run(["docker", "rm", "-f", container], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    captures = []
    for mode in modes:
        session = output / mode / "session"
        if list((session / "plugin-storage").glob("*/Synths")):
            raise RuntimeError("Selecting a plugin location created a content folder")
        if list((output / mode).glob("MPC.settings.pmu-*")):
            raise RuntimeError("Selecting a plugin location changed settings")
        for path in session.glob("*.bmp"):
            captures.append(str(compact_capture(path).relative_to(output)))
    (output / "results.json").write_text(json.dumps({"date": time.strftime("%d-%m-%Y"),
        "scope": "Isolated ARM64 UI fixtures; no hardware operations", "passed": list(modes),
        "captures": captures}, indent=2) + "\n")
    print("PASS: Gen2 automatic/manual location controls, blocked-drive recovery and screenshots")


if __name__ == "__main__":
    main()

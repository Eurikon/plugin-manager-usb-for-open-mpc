import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from prepare_fixtures import _prepare, catalog_item, package_manifest


def main():
    parser = argparse.ArgumentParser(description="Test real package install recovery inside an isolated build container")
    parser.add_argument("package", type=Path, help="Your trusted ARMv7 OS 3 plugin ZIP")
    parser.add_argument("backend_test", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("--ui-mode", choices=("browser", "browser-dark", "browser-touch"))
    parser.add_argument("--font", type=Path)
    parser.add_argument("--panel-input-library", type=Path)
    parser.add_argument("--full-backend", action="store_true")
    args = parser.parse_args()
    if args.full_backend and args.ui_mode:
        parser.error("Choose the full backend suite or a UI mode")
    if os.geteuid() != 0 or not Path("/.dockerenv").is_file():
        parser.error("Run as root inside an isolated offline Docker container")
    chroot = shutil.which("chroot")
    if chroot is None:
        parser.error("The build container needs chroot")
    package = args.package.resolve(strict=True)
    primary = catalog_item(package, package_manifest(package))
    root = Path(tempfile.mkdtemp(prefix="pmu-plugin-recovery-"))
    mounted = []

    def run(*command):
        subprocess.run(command, check=True, capture_output=True, text=True)

    try:
        (root / "proc").mkdir()
        _prepare(root, package, None, primary, None)
        if args.ui_mode:
            if args.font is None or not args.font.is_file():
                parser.error("UI checks require --font pointing to the manager font")
            shutil.copyfile(args.font, root / "tmp/manager-font.ttf")
            if args.ui_mode == "browser-dark":
                (root / "tmp/manager-usb/manager-preferences.json").write_text(json.dumps({"schema": 1, "dark_theme": True, "hide_welcome": False}) + "\n")
        for directory in ("usr", "lib", "lib64", "bin", "sbin", "etc", "dev"):
            source = Path("/") / directory
            if not source.exists():
                continue
            target = root / directory
            if target.exists():
                shutil.rmtree(target)
            if source.is_symlink():
                target.symlink_to(source.readlink())
            else:
                target.mkdir()
                run("mount", "--bind", str(source), str(target))
                mounted.append(target)
                if directory != "dev":
                    run("mount", "-o", "remount,bind,ro", str(target))
        live_proc = root / "real-proc"
        live_proc.mkdir()
        run("mount", "-t", "proc", "proc", str(live_proc))
        mounted.append(live_proc)
        (root / "proc/self").symlink_to("../real-proc/self")
        binary = root / "tmp/backend-test"
        shutil.copyfile(args.backend_test, binary)
        binary.chmod(0o700)
        environment = dict(os.environ, PATH="/tmp/manager-test-bin:/usr/bin:/bin", MPC_INSTALL_TEST="1")
        arguments = [] if args.full_backend else ["--plugin-recovery"]
        if args.ui_mode:
            environment.update(SDL_VIDEODRIVER="dummy", MPC_MANAGER_UI_TEST=args.ui_mode)
            if args.panel_input_library:
                shutil.copyfile(args.panel_input_library, root / "tmp/panel-input-fake.so")
                environment.update(MPC_PANEL_PORT="hw:1,0,1", MPC_MANAGER_PANEL_FIXTURE="1")
                panel = root / "sys/devices/panel"
                (panel / "interface").mkdir(parents=True)
                (panel / "idVendor").write_text("09e8\n")
                (panel / "idProduct").write_text("0047\n")
                card = root / "sys/class/sound/card1"
                card.mkdir(parents=True)
                (card / "device").symlink_to("/sys/devices/panel/interface")
            arguments = ["/tmp/manager-usb", "/tmp/manager-session", "/sdcard/Expansions", "/tmp/manager-settings.xml", "/tmp/manager-font.ttf"]
        args.log.parent.mkdir(parents=True, exist_ok=True)
        def expose_process():
            process = str(os.getpid())
            (root / "proc" / process).symlink_to("../real-proc/" + process)

        with args.log.open("w") as log:
            inner = (["/usr/bin/env", "LD_PRELOAD=/tmp/panel-input-fake.so"] if args.panel_input_library else [])
            result = subprocess.run([chroot, str(root), *inner, "/tmp/backend-test", *arguments], env=environment,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=180, preexec_fn=expose_process)
        if args.ui_mode:
            from sd_ui_test import compact_capture
            captures = args.log.parent / (args.log.stem + "-captures")
            captures.mkdir(exist_ok=False)
            for source in (root / "tmp/manager-session").glob("*.bmp"):
                destination = captures / source.name
                shutil.copyfile(source, destination)
                compact_capture(destination)
        if result.returncode:
            raise RuntimeError(f"{'Catalog UI' if args.ui_mode else 'Plugin recovery backend'} check failed; see {args.log}")
        print("PASS: " + ("catalog UI navigation and installation" if args.ui_mode else "actual install/update/uninstall recovery") + "; " + str(args.log))
    finally:
        for target in reversed(mounted):
            subprocess.run(["umount", str(target)], check=True)
        shutil.rmtree(root)


if __name__ == "__main__":
    main()

import argparse
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile


def run(arguments, timeout=120):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError("Command failed: " + " ".join(map(str, arguments)) + "\n" + result.stdout + result.stderr)
    return result


def script(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        path.unlink()
    path.write_text("#!/bin/sh\nset -eu\n" + text)
    path.chmod(0o700)


def main():
    parser = argparse.ArgumentParser(description="Run signed-manager stock-library checks inside an isolated Docker container")
    for name in ("firmware", "bundle", "probe", "binaries", "logs"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--arch", choices=("armv7", "aarch64"), required=True)
    parser.add_argument("--bootstrap", action="store_true", help="Check the real OS 3.10 helper preparation script")
    parser.add_argument("--lifecycle-source", type=Path, help="Readonly source subset containing scripts and shortcut lifecycle tests")
    parser.add_argument("--extractor", type=Path, default=Path("/tools/extract_firmware.py"))
    args = parser.parse_args()
    if not Path("/.dockerenv").is_file() or os.geteuid() != 0:
        parser.error("Run only as root inside an isolated Docker container")
    spec = importlib.util.spec_from_file_location("stock_extract", args.extractor)
    extractor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extractor)
    temporary = Path(tempfile.mkdtemp(prefix="pmu-stock-update-"))
    root = temporary / "root"
    root.mkdir()
    mounts = []
    processes = []
    report = {"date": datetime.now().strftime("%d-%m-%Y"), "architecture": args.arch,
              "scope": "Stock libraries and signed noexec startup; MIDI/services simulated; helper API queried without MPC callbacks",
              "hardware_operations": False}
    args.logs.mkdir(parents=True, exist_ok=True)
    try:
        filesystem = temporary / "root.ext"
        extractor.extract_rootfs(args.firmware, filesystem)
        run(["debugfs", "-R", f'rdump / "{root}"', filesystem], timeout=180)
        filesystem.unlink()
        for name in ("proc", "dev", "tmp", "run", "media/PMU_AUDIT"):
            (root / name).mkdir(parents=True, exist_ok=True)
        for source, target, flags in (("tmpfs", root / "tmp", ["-t", "tmpfs", "-o", "exec,nosuid,nodev,size=64m"]),
                                      ("tmpfs", root / "run", ["-t", "tmpfs", "-o", "noexec,nosuid,nodev,size=4m"]),
                                      ("proc", root / "proc", ["-t", "proc"]),
                                      ("/dev", root / "dev", ["--rbind"]),
                                      ("tmpfs", root / "media/PMU_AUDIT", ["-t", "tmpfs", "-o", "noexec,nosuid,nodev,size=64m"])):
            run(["mount", *flags, source, target])
            mounts.append(target)
        source = root / "media/PMU_AUDIT"
        if not os.statvfs(source).f_flag & os.ST_NOEXEC:
            raise RuntimeError("The manager source must be mounted noexec")
        if any(path.is_symlink() for path in args.bundle.rglob("*")):
            raise RuntimeError("The supplied factory bundle contains a link")
        shutil.copytree(args.bundle, source, dirs_exist_ok=True)
        helper_relative = ("nvme" if args.arch == "aarch64" else "sdcard") + "/MPC Documents/Devices/Plugin Manager helper"
        helper = root / helper_relative
        if not helper.resolve().is_relative_to(root.resolve()):
            raise RuntimeError("The helper fixture escapes its disposable root")
        if (source / "Plugin Manager helper.xpa").stat().st_size > 8 * 1024 * 1024:
            raise RuntimeError("Oversized helper archive")
        with zipfile.ZipFile(source / "Plugin Manager helper.xpa") as archive:
            entries = archive.infolist()
            if len(entries) > 256 or sum(entry.file_size for entry in entries) > 16 * 1024 * 1024:
                raise RuntimeError("Oversized expanded helper archive")
            for entry in archive.infolist():
                relative = Path(entry.filename).relative_to("Devices/Plugin Manager helper")
                if relative.is_absolute() or ".." in relative.parts or entry.is_dir() or entry.flag_bits & 1 or entry.file_size > 2 * 1024 * 1024:
                    raise RuntimeError("Unsafe helper archive member")
                destination = helper / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.read(entry))
                destination.chmod(0o600)
        helper.chmod(0o700)
        (helper / ".pmu-versions").chmod(0o700)
        audit = root / "tmp/pmu-runtime-audit"
        audit.mkdir(mode=0o700)
        shutil.copyfile(args.probe, audit / "probe")
        (audit / "probe").chmod(0o700)
        shutil.copyfile(source / ("autorun/runtime/source-dispatcher-" + args.arch), audit / "dispatcher")
        (audit / "dispatcher").chmod(0o700)
        chroot = ["chroot", str(root)]
        check = run([*chroot, "/tmp/pmu-runtime-audit/probe", "/media/PMU_AUDIT", "/" + helper_relative])
        (args.logs / "native-probe.log").write_text(check.stdout + check.stderr)
        for name in ("first", "second"):
            shutil.copyfile(args.binaries / ("shortcut-addin-" + name + ".so"), audit / (name + ".so"))
        preload = "/tmp/pmu-runtime-audit/first.so /tmp/pmu-runtime-audit/second.so"
        mpc = subprocess.Popen([*chroot, "/usr/bin/busybox.nosuid", "env", "LD_PRELOAD=" + preload,
                                "/usr/bin/busybox.nosuid", "sleep", "180"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        processes.append(mpc)
        (audit / "main-pid").write_text(str(mpc.pid) + "\n")
        (audit / "existing.env").write_text('LD_PRELOAD="' + preload + '"\n')
        script(root / "usr/bin/amidi", "printf 'Dir Device Name\\nIO hw:1,0,1 Test Private\\n'\n")
        script(root / "usr/bin/systemctl", '''case "$1" in
cat|daemon-reload) exit 0 ;;
show)
 case "$*" in
 'show acvs -p MainPID --value') cat /tmp/pmu-runtime-audit/main-pid ;;
 'show acvs -p Environment --value'|'show acvs -p UnsetEnvironment --value') printf '\\n' ;;
 'show acvs -p EnvironmentFiles --value')
   printf '/tmp/pmu-runtime-audit/existing.env (ignore_errors=no)'
   if [ -f /run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf ]; then
     IFS= read -r base < /run/mpc-usb-manager-session
     printf ' %s/shortcut.env (ignore_errors=no)' "$base"
   fi
   printf '\\n' ;;
 *) exit 8 ;;
 esac ;;
is-active) exit 0 ;;
*) exit 9 ;;
esac
''')
        script(root / "usr/bin/systemd-run", '''while [ "$#" -gt 0 ]; do
 case "$1" in --setenv=MPC_MANAGER_SD_HELPER=*) export "${1#--setenv=}"; shift ;; --*) shift ;; *) break ;; esac
done
[ "$#" -ge 5 ]
"$@" </dev/null >/dev/null 2>&1 &
echo "$!" > /tmp/pmu-runtime-audit/supervisor-pid
''')
        helper_name = "/" + helper_relative if args.bootstrap else ""
        if args.bootstrap:
            selection = json.loads((helper / ".pmu-versions/active.json").read_text())
            generation = selection["active"]
            if len(generation) != 64 or any(byte not in "0123456789abcdef" for byte in generation):
                raise RuntimeError("Invalid helper generation")
            payload = helper_name + "/.pmu-versions/" + generation
            permission = root / "run/open-mpc-manager-preparation.Audit1"
            permission.mkdir(mode=0o700)
            (permission / "request").write_text("open-mpc-manager-prepare-v1\n")
            (permission / "request").chmod(0o600)
            source_stat, marker_stat = source.stat(), (source / ".mpc-manager-id").stat()
            identity = f"{source_stat.st_dev}:{source_stat.st_ino}:{marker_stat.st_dev}:{marker_stat.st_ino}"
            prepared = run([*chroot, "/bin/sh", payload + "/bootstrap.sh", "--prepare", "/media/PMU_AUDIT",
                            identity, "/run/" + permission.name + "/request", "--pmu-helper-v1", helper_name, payload])
            (args.logs / "bootstrap.log").write_text(prepared.stdout + prepared.stderr)
            session = (permission / "session").read_text()
            if (permission / "ready").read_text() != session:
                raise RuntimeError("The actual helper bootstrap did not publish matching readiness")
            prepared = subprocess.CompletedProcess([], 0, session, "")
        else:
            prepared = run([*chroot, "/tmp/pmu-runtime-audit/dispatcher", "/media/PMU_AUDIT", "", "prepare"])
        (args.logs / "prepare.log").write_text(prepared.stdout + prepared.stderr)
        base = Path(prepared.stdout.strip())
        if base.parent != Path("/tmp") or not base.name.startswith("mpc-usb-session."):
            raise RuntimeError("The dispatcher returned an invalid RAM cohort")
        cohort = root / base.relative_to("/")
        information = json.loads(run([*chroot, str(base / "manager"), "--pmu-info-v1"]).stdout)
        expected_version = json.loads((source / "PMU-UPDATE.json").read_text())["generation"]["version"]
        if information.get("version") != expected_version or information.get("architecture") != args.arch or information.get("development") is not False:
            raise RuntimeError("The release binary has a mismatched version, architecture or development identity")
        report["native_identity"] = information
        if (cohort / "ready").read_text() != "READY\n":
            raise RuntimeError("The native supervisor did not report readiness")
        environment = (cohort / "shortcut.env").read_text()
        if not environment.index("first.so") < environment.index("second.so") < environment.index("shortcut-hook.so"):
            raise RuntimeError("Existing addin ordering changed")
        again = run([*chroot, "/tmp/pmu-runtime-audit/dispatcher", "/media/PMU_AUDIT", helper_name, "prepare"])
        if again.stdout.strip() != str(base):
            raise RuntimeError("Quiet preparation replaced an existing cohort")
        work = Path(tempfile.mkdtemp(prefix="mpc-usb-manager.stock.", dir=root / "tmp"))
        work.chmod(0o700)
        for name in ("manager", "runtime-context.json"):
            shutil.copyfile(cohort / name, work / name)
            (work / name).chmod(0o700 if name == "manager" else 0o600)
        settings = root / ("data/Settings/MPC/MPC.settings" if args.arch == "aarch64" else "media/az01-internal/Settings/MPC/MPC.settings")
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("<PROPERTIES/>\n")
        target = root / ("nvme/Synths" if args.arch == "aarch64" else "sdcard/Expansions")
        target.mkdir(parents=True, exist_ok=True)
        script(root / "usr/bin/wget", "exit 1\n")
        gui = run([*chroot, "/usr/bin/busybox.nosuid", "env", "SDL_VIDEODRIVER=dummy", "MPC_MANAGER_UI_TEST=smoke",
                   "/" + str((work / "manager").relative_to(root)), "--pmu-gui-v1", "/media/PMU_AUDIT",
                   "/" + str((work / "cache").relative_to(root)), "/" + str(target.relative_to(root)),
                   "/" + str(settings.relative_to(root)), str(base / "font.ttf")])
        (args.logs / "gui.log").write_text(gui.stdout + gui.stderr)
        shutil.copyfile(work / "cache/screen.bmp", args.logs / "first-frame.bmp")
        shutil.rmtree(work)
        run([*chroot, str(base / "shortcut"), "--handoff", str(base / "shortcut.sock")])
        deadline = time.monotonic() + 15
        while cohort.exists() and time.monotonic() < deadline:
            time.sleep(0.025)
        if cohort.exists() or (root / "run/mpc-usb-manager-session").exists():
            raise RuntimeError("The supervisor left owned RAM state behind")
        if args.lifecycle_source:
            run(["umount", str(root / "proc")])
            mounts.remove(root / "proc")
            lifecycle = run([sys.executable, args.lifecycle_source / "tests/shortcut_lifecycle.py", root,
                             "--direct", "--build-directory", args.binaries], timeout=240)
            (args.logs / "shortcut-lifecycle.log").write_text(lifecycle.stdout + lifecycle.stderr)
        report.update(passed=True, noexec_source=True, quiet_reuse=True, addin_order_preserved=True,
                      actual_gui_loader=True, native_helper_abi=True, actual_bootstrap=args.bootstrap,
                      shortcut_lifecycle=bool(args.lifecycle_source))
        print("PASS: stock ABI, signed noexec startup, addin ordering, quiet reuse, actual GUI loader and RAM cleanup", flush=True)
    except Exception as error:
        report.update(passed=False, error=str(error))
        raise
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=10)
        for target in reversed(mounts):
            subprocess.run(["umount", "-R", str(target)], capture_output=True)
            if os.path.ismount(target):
                subprocess.run(["umount", "-l", str(target)], capture_output=True)
        (args.logs / "runtime.json").write_text(json.dumps(report, indent=2) + "\n")
        if not any(os.path.ismount(target) for target in mounts):
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()

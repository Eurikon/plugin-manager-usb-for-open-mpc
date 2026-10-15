import argparse
import datetime
import hashlib
import json
import shutil
import subprocess
import sys
import uuid
import zipfile
from pathlib import Path

from offline_support import ChrootRunner, add_runtime_arguments, disposable_root, execute
from prepare_fixtures import catalog_item, digest, package_manifest, prepare


SOURCE = Path(__file__).resolve().parents[1]
BINARIES = ("manager", "backend-test", "font.ttf", "shortcut", "shortcut-hook.so",
            "shortcut-state-test", "shortcut-fake.so", "shortcut-hook-test", "shortcut-integration")


def preflight(root, package, legacy_package=None, *, legacy_os3_confirmed=False):
    root = disposable_root(root)
    package = Path(package).resolve()
    legacy_package = Path(legacy_package).resolve() if legacy_package else None
    if legacy_package is not None and not legacy_os3_confirmed:
        raise ValueError("Confirm legacy package OS 3 compatibility with --legacy-os3-confirmed")
    if legacy_os3_confirmed and legacy_package is None:
        raise ValueError("--legacy-os3-confirmed requires --legacy-package")
    for path in (package, legacy_package):
        if path is not None and path.is_relative_to(root):
            raise ValueError("Input ZIPs must be outside the disposable fixture root")
    primary = package_manifest(package)
    legacy = package_manifest(legacy_package, legacy=True) if legacy_package else None
    catalog_item(package, primary)
    if legacy:
        catalog_item(legacy_package, legacy, legacy=True)
    return root, package, legacy_package, primary, legacy


def stage(root, package, legacy_package=None):
    prepare(root, package, legacy_package, legacy_os3_confirmed=legacy_package is not None)
    for source, target in (("manager", "manager-ui"), ("backend-test", "manager-backend-test"), ("font.ttf", "manager-font.ttf")):
        destination = root / "tmp" / target
        if destination.exists() or destination.is_symlink():
            destination.unlink()
        shutil.copyfile(SOURCE / "build" / source, destination)
        destination.chmod(0o755 if source != "font.ttf" else 0o644)


def build_command(image, direct):
    if direct:
        return ["sh", str(SOURCE / "build.sh")]
    return ["docker", "run", "--rm", "--pull", "never", "--name", "mpc-manager-build-" + uuid.uuid4().hex,
            "--platform", "linux/arm/v7", "--network", "none", "--mount",
            f"type=bind,src={SOURCE},dst=/work", "--workdir", "/work", image, "sh", "build.sh"]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Offline manager tests using disposable stock firmware, mock downloads and service control")
    parser.add_argument("root", type=Path)
    parser.add_argument("package_zip", type=Path, help="Your own trusted ARMv7 OS 3 deferred-install package ZIP")
    parser.add_argument("--legacy-package", type=Path, help="Optional trusted package lacking os_compat")
    parser.add_argument("--legacy-os3-confirmed", action="store_true", help="Confirm supplied legacy package is compatible with OS 3")
    add_runtime_arguments(parser)
    parser.add_argument("--skip-build", action="store_true", help="Use existing build outputs")
    parser.add_argument("--logs", type=Path, default=SOURCE / "build/offline-results")
    args = parser.parse_args(argv)
    root, package, legacy_package, primary, legacy = preflight(args.root, args.package_zip, args.legacy_package, legacy_os3_confirmed=args.legacy_os3_confirmed)
    runner = ChrootRunner(root, args.image, args.direct, args.timeout)
    args.logs.mkdir(parents=True, exist_ok=True)
    results = {"date": datetime.datetime.now().strftime("%d-%m-%Y"), "scope": "offline stock firmware chroot; mock downloads and service control",
               "package": {"id": primary["id"], "version": primary["version"], "sha256": digest(package)},
               "legacy_package": {"id": legacy["id"], "version": legacy["version"], "sha256": digest(legacy_package)} if legacy else None,
               "steps": []}

    def step(name, command):
        log = args.logs / (name + ".log")
        try:
            result = execute(command, args.timeout)
        except subprocess.TimeoutExpired as error:
            log.write_text((error.stdout or "") + (error.stderr or "") + f"\nTIMEOUT after {args.timeout} seconds\n")
            raise RuntimeError(f"{name} timed out; see {log}") from error
        log.write_text(result.stdout + result.stderr)
        output = result.stdout + result.stderr
        skipped_cases = [line for line in output.splitlines() if line.startswith("SKIP:")]
        checks = output.count("PASS:")
        results["steps"].append({"name": name, "returncode": result.returncode, "checks": checks, "skipped": len(skipped_cases), "skipped_cases": skipped_cases})
        (args.logs / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{name} failed (status {result.returncode}); see {log}")
        for line in skipped_cases:
            print(line, flush=True)
        print(f"PASS: {name}; {checks} checks passed, {len(skipped_cases)} skipped; log {log}", flush=True)

    if not args.skip_build:
        step("build", build_command(args.image, args.direct))
    for name in BINARIES:
        if not (SOURCE / "build" / name).is_file():
            raise ValueError(f"Missing build/{name}; run without --skip-build")
    results["manager_sha256"] = hashlib.sha256((SOURCE / "build/manager").read_bytes()).hexdigest()
    stage(root, package, legacy_package)
    step("backend", runner.command("PATH=/tmp/manager-test-bin:$PATH MPC_INSTALL_TEST=1 /tmp/manager-backend-test"))
    for mode in ("flow", "touch-flow"):
        stage(root, package, legacy_package)
        step(mode, runner.command(f"PATH=/tmp/manager-test-bin:$PATH MPC_INSTALL_TEST=1 SDL_VIDEODRIVER=dummy MPC_MANAGER_UI_TEST={mode} /tmp/manager-ui /tmp/manager-usb /tmp/manager-session /sdcard/Expansions /tmp/manager-settings.xml /tmp/manager-font.ttf"))
        shutil.copyfile(root / "tmp/manager-session/screen.bmp", args.logs / (mode + ".bmp"))
    for name in ("lifecycle_test", "shortcut_lifecycle"):
        stage(root, package, legacy_package)
        command = [sys.executable, str(SOURCE / "tests" / (name + ".py")), str(root), "--image", args.image, "--timeout", str(args.timeout)]
        if args.direct:
            command.append("--direct")
        step(name, command)
    skipped = sum(step["skipped"] for step in results["steps"])
    print(f"PASS: offline suites completed; {skipped} skipped cases; hardware behavior remains a separate check", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, RuntimeError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error)) from error

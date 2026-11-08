"""Build and publish manager archives only after older-updater acceptance checks."""

import argparse
from datetime import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


ARCHITECTURES = ("armv7", "aarch64")
IMAGES = {"armv7": "mpc-usb-plugin-installer-build-tools",
          "aarch64": "mpc-usb-plugin-installer-build-tools:aarch64"}


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            value.update(block)
    return value.hexdigest()


def run(arguments, timeout=180):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise ValueError(result.stderr.strip() or result.stdout.strip() or "Release command failed")
    return result.stdout


def files_for_bundle(root):
    root = Path(root).resolve()
    paths = list(root.rglob("*"))
    if any(path.is_symlink() or not (path.is_file() or path.is_dir()) for path in paths):
        raise ValueError("Release bundle contains a link or special file")
    envelope = json.loads((root / "PMU-UPDATE.json").read_text())
    if set(envelope) != {"protocol", "product", "generation", "authorization"} or envelope["protocol"] != 1 or envelope["product"] != "plugin-manager-usb":
        raise ValueError("Unsupported manager update envelope")
    spec = envelope["generation"]
    version = spec["version"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version) or spec["architecture"] != "universal":
        raise ValueError("A public release requires a numbered universal bundle")
    files = {path.relative_to(root).as_posix() for path in paths if path.is_file()}
    if files != set(spec["files"]) | {"PMU-UPDATE.json"}:
        raise ValueError("Release bundle has missing or uninventoried files")
    directories = set()
    for name in files:
        for parent in Path(name).parents:
            if parent != Path("."):
                directories.add(parent.as_posix())
    if any(path.is_dir() and path.relative_to(root).as_posix() not in directories | {"local-plugins"} for path in paths):
        raise ValueError("Release bundle contains an unrelated directory")
    if not (root / "local-plugins").is_dir() or any((root / "local-plugins").iterdir()):
        raise ValueError("Public bundle requires an empty local-plugins folder")
    for name, expected in spec["files"].items():
        if digest(root / name) != expected:
            raise ValueError("Signed release checksum mismatch: " + name)
    inventory = json.loads((root / "USB-SHA256.json").read_text())
    if set(inventory) != files - {"PMU-UPDATE.json", "USB-SHA256.json"} or any(inventory[name] != spec["files"][name] for name in inventory):
        raise ValueError("Factory inventory differs from signed release")
    return files, version


def write_archive(bundle, archive):
    files, version = files_for_bundle(bundle)
    archive = Path(archive)
    if archive.name != "plugin-manager-usb-v" + version + ".zip":
        raise ValueError("Release ZIP filename must match its signed version")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for name in sorted(files):
            output.write(Path(bundle) / name, name)
    with zipfile.ZipFile(archive) as output:
        if output.testzip() is not None or set(output.namelist()) != files:
            raise ValueError("Final ZIP differs from the signed inventory")
    return version


def validate(archive, version, validators):
    archive = Path(archive).resolve()
    expected = digest(archive)
    reports = {}
    with tempfile.TemporaryDirectory(prefix="pmu-release-check-") as temporary:
        bad = Path(temporary) / archive.name
        shutil.copyfile(archive, bad)
        with zipfile.ZipFile(bad, "a") as output:
            output.writestr("local-plugins/", b"")
        for arch in ARCHITECTURES:
            validator = validators[arch]
            executable = Path(validator["path"]).resolve()
            if executable.is_symlink() or not executable.is_file() or digest(executable) != validator["sha256"]:
                raise ValueError("Baseline validator changed: " + arch)
            image = validator["image"]
            if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
                raise ValueError("Release checks require a pinned Docker image")
            def command(path, checksum):
                return ["docker", "run", "--rm", "--pull=never", "--platform=linux/" + ("arm/v7" if arch == "armv7" else "arm64"),
                        "--network=none", "--read-only", "--memory=768m", "--pids-limit=128", "--tmpfs=/tmp:rw,exec,size=536870912",
                        "--mount", "type=bind,src=" + str(executable) + ",dst=/validator,readonly",
                        "--mount", "type=bind,src=" + str(path) + ",dst=/input/release.zip,readonly",
                        image, "/validator", "/input/release.zip", version, arch, checksum]
            rejected = subprocess.run(command(bad, digest(bad)), capture_output=True, text=True, timeout=180)
            if rejected.returncode == 0 or "uninventoried content" not in rejected.stderr:
                raise ValueError("Baseline did not reproduce the empty-directory rejection: " + arch + ": " + rejected.stderr)
            result = json.loads(run(command(archive, expected)))
            if (result.get("schema") != 1 or result.get("architecture") != arch or result.get("version") != version
                    or result.get("archive_sha256") != expected or not all(result.get(key) is True for key in
                        ("download_checked", "signed_archive_checked", "native_worker_ready", "installation_unchanged"))):
                raise ValueError("Incomplete older-updater acceptance report: " + arch)
            result["empty_directory_rejected"] = True
            reports[arch] = result
    if digest(archive) != expected:
        raise ValueError("Release ZIP changed during validation")
    return reports


def receipt_path(archive):
    return Path(str(archive) + ".validation.json")


def validate_lifecycle(paths, expected=None):
    if not paths or set(paths) != set(ARCHITECTURES):
        raise ValueError("Both architecture update-return-reopen reports are required")
    project = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("pmu_update_core_inputs", project / "tests/run_update_core.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    inputs = {name: digest(project / name) for name in module.SOURCES}
    results = {}
    for arch in ARCHITECTURES:
        path = Path(paths[arch]).resolve()
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise ValueError("Missing or oversized lifecycle report: " + arch)
        checksum = digest(path)
        if expected and checksum != expected[arch]["sha256"]:
            raise ValueError("Lifecycle report changed: " + arch)
        value = json.loads(path.read_text())
        if (value.get("arch") != arch or value.get("passed") is not True or value.get("returncode") != 0
                or value.get("update_lifecycle") != {"sd": True, "usb": True}
                or value.get("files") != inputs or value.get("checks_by_suite", {}).get("runtime-handoff", 0) <= 0):
            raise ValueError("Missing, failed or stale update-return-reopen validation: " + arch)
        log = path.parent / "tests.log"
        if not log.is_file() or log.stat().st_size > 4 * 1024 * 1024 or digest(log) != value.get("tests_sha256"):
            raise ValueError("Lifecycle test log changed: " + arch)
        text = log.read_text()
        if any(text.count("PASS: update-return-reopen " + medium + " with concurrent helper confirmation\n") != 1 for medium in ("sd", "usb")):
            raise ValueError("Incomplete update-return-reopen test log: " + arch)
        results[arch] = {"path": str(path), "sha256": checksum, "tests_sha256": digest(log)}
    return results


def prepare(bundle, archive, validator_paths, images=IMAGES, *, lifecycle_reports=None):
    lifecycle = validate_lifecycle(lifecycle_reports)
    version = write_archive(bundle, archive)
    validators = {}
    for arch in ARCHITECTURES:
        path = Path(validator_paths[arch]).resolve()
        image = run(["docker", "image", "inspect", "--format={{.Id}}", images[arch]]).strip()
        validators[arch] = {"path": str(path), "sha256": digest(path), "image": image}
    reports = validate(archive, version, validators)
    checksum = digest(archive)
    Path(str(archive) + ".sha256").write_text(checksum + "  " + Path(archive).name + "\n")
    receipt = {"schema": 1, "date": datetime.now().strftime("%d-%m-%Y"), "version": version,
               "archive_sha256": checksum, "validators": validators, "reports": reports, "lifecycle": lifecycle}
    receipt_path(archive).write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def checked_receipt(archive):
    archive = Path(archive)
    receipt = json.loads(receipt_path(archive).read_text())
    if (receipt.get("schema") != 1 or receipt.get("archive_sha256") != digest(archive)
            or archive.name != "plugin-manager-usb-v" + receipt.get("version", "") + ".zip"
            or set(receipt.get("validators", {})) != set(ARCHITECTURES)
            or set(receipt.get("lifecycle", {})) != set(ARCHITECTURES)):
        raise ValueError("Release ZIP has no matching final-archive acceptance receipt")
    return receipt


def publish(archive, repository, notes):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid release repository")
    receipt = checked_receipt(archive)
    validate_lifecycle({arch: value["path"] for arch, value in receipt["lifecycle"].items()}, receipt["lifecycle"])
    with tempfile.TemporaryDirectory(prefix="pmu-release-upload-") as temporary:
        root = Path(temporary)
        upload = root / Path(archive).name
        shutil.copyfile(archive, upload)
        upload.chmod(stat.S_IRUSR)
        if digest(upload) != receipt["archive_sha256"]:
            raise ValueError("Upload copy differs from the validated release")
        validate(upload, receipt["version"], receipt["validators"])
        checksum = Path(str(upload) + ".sha256")
        checksum.write_text(receipt["archive_sha256"] + "  " + upload.name + "\n")
        tag = "v" + receipt["version"]
        run(["gh", "release", "create", tag, upload, checksum, "--repo", repository, "--verify-tag", "--title", receipt["version"], "--notes-file", notes, "--draft"])
        downloaded = root / "downloaded"
        downloaded.mkdir()
        run(["gh", "release", "download", tag, "--repo", repository, "--dir", downloaded, "--pattern", upload.name + "*"])
        if digest(downloaded / upload.name) != receipt["archive_sha256"] or (downloaded / checksum.name).read_bytes() != checksum.read_bytes():
            raise ValueError("Draft release assets differ; release remains unpublished")
        run(["gh", "release", "edit", tag, "--repo", repository, "--draft=false", "--latest"])
    return "https://github.com/" + repository + "/releases/tag/" + tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("prepare")
    create.add_argument("bundle", type=Path)
    create.add_argument("archive", type=Path)
    for arch in ARCHITECTURES:
        create.add_argument("--validator-" + arch, required=True, type=Path)
        create.add_argument("--lifecycle-" + arch, required=True, type=Path)
    upload = commands.add_parser("publish")
    upload.add_argument("archive", type=Path)
    upload.add_argument("--repo", required=True)
    upload.add_argument("--notes-file", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            prepare(args.bundle, args.archive, {arch: getattr(args, "validator_" + arch) for arch in ARCHITECTURES},
                    lifecycle_reports={arch: getattr(args, "lifecycle_" + arch) for arch in ARCHITECTURES})
            print("Final ZIP accepted by both older-updater validators; checksum and receipt saved.")
        else:
            print(publish(args.archive, args.repo, args.notes_file))
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

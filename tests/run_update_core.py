import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("src/checksum.hpp", "src/checksum.cpp", "src/version_store.hpp",
           "src/version_store.cpp", "src/update_worker.hpp", "src/update_worker.cpp",
           "tests/version_store_test.cpp", "tests/update_worker_fixture.cpp", "tests/update_worker_test.cpp")


def main():
    parser = argparse.ArgumentParser(description="Validate the update core in an existing offline native build image")
    parser.add_argument("--arch", choices=("armv7", "aarch64"), default="aarch64")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Use a fresh output directory inside build/")
    output.mkdir(parents=True, exist_ok=False)
    data = {name: (ROOT / name).read_bytes() for name in SOURCES}
    fingerprint = hashlib.sha256(b"".join(name.encode() + b"\0" + value for name, value in data.items())).hexdigest()
    source = output / "source"
    for name, value in data.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)
    image = "mpc-usb-plugin-installer-build-tools" + (":aarch64" if args.arch == "aarch64" else "")
    flags = ["-O0", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"]
    compiler = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags, "-I/source/src"]
    commands = [" ".join([*compiler, "-c", "/source/src/" + name + ".cpp", "-o", "/results/" + name + ".o"])
                for name in ("checksum", "version_store", "update_worker")]
    objects = ["/results/" + name + ".o" for name in ("checksum", "version_store", "update_worker")]
    for source_name, binary in (("version_store_test.cpp", "version-store-test"),
                                ("update_worker_fixture.cpp", "worker-fixture"),
                                ("update_worker_test.cpp", "update-worker-test")):
        commands.append(" ".join([*compiler, "/source/tests/" + source_name, *objects, "-o", "/results/" + binary]))
    commands.extend(("/results/version-store-test", "/results/update-worker-test /results/worker-fixture"))
    container = "pmu-update-core-" + uuid.uuid4().hex[:12]
    command = ["docker", "run", "--rm", "--pull=never", "--name", container,
               "--platform=" + ("linux/arm64" if args.arch == "aarch64" else "linux/arm/v7"),
               "--network=none", "--read-only", "--memory=768m", "--pids-limit=128",
               "--tmpfs=/tmp:rw,exec,size=67108864", "--mount", f"type=bind,src={source},dst=/source,readonly",
               "--mount", f"type=bind,src={output},dst=/results", image, "sh", "-ec",
               "; ".join(commands)]
    report = {"date": datetime.now().strftime("%d-%m-%Y"), "arch": args.arch, "sanitizers": args.sanitize,
              "source_sha256": fingerprint, "files": {name: hashlib.sha256(value).hexdigest() for name, value in data.items()},
              "scope": "Offline native core; no hardware or media operations", "command": command}
    try:
        with (output / "tests.log").open("x") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        text = (output / "tests.log").read_text()
        report["returncode"] = result.returncode
        report["passed"] = result.returncode == 0 and not re.search(r"warning:|runtime error:|AddressSanitizer:|^FAIL:", text, re.M)
        counts = {name: int(count) for count, name in re.findall(r"PASS: (\d+) (version-store|update-worker)", text)}
        report["checks_by_suite"] = counts
        report["checks"] = sum(counts.values())
        if not report["passed"] or set(counts) != {"version-store", "update-worker"}:
            raise RuntimeError("Update-core verification failed; inspect " + str(output / "tests.log"))
        print(f"PASS: {args.arch}, {report['checks']} checks" + (", fatal ASan/UBSan" if args.sanitize else ""))
    except Exception as error:
        report["passed"] = False
        report["error"] = str(error)
        raise
    finally:
        subprocess.run(["docker", "rm", "-f", container], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()

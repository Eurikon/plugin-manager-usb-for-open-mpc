import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("src/checksum.hpp", "src/checksum.cpp", "src/native_image.hpp", "src/native_image.cpp", "src/version_store.hpp",
           "src/version_store.cpp", "src/update_worker.hpp", "src/update_worker.cpp",
           "src/update_bundle.hpp", "src/update_bundle.cpp", "tests/update_bundle_test.cpp",
           "src/helper_payload.h", "src/helper_loader.hpp", "src/helper_loader.cpp",
           "tests/helper_loader_fixture.h", "tests/helper_loader_fixture.c", "tests/helper_loader_test.cpp",
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
                for name in ("checksum", "native_image", "version_store", "update_worker", "update_bundle", "helper_loader")]
    objects = ["/results/" + name + ".o" for name in ("checksum", "native_image", "version_store", "update_worker")]
    for source_name, binary in (("version_store_test.cpp", "version-store-test"),
                                ("update_worker_fixture.cpp", "worker-fixture"),
                                ("update_worker_test.cpp", "update-worker-test"),
                                ("update_bundle_test.cpp", "update-bundle-test")):
        bundle = ["/results/update_bundle.o", "-larchive"] if binary == "update-bundle-test" else []
        commands.append(" ".join([*compiler, "/source/tests/" + source_name, *objects, *bundle, "-o", "/results/" + binary]))
    for version, abi in ((1, 1), (2, 1), (3, 99)):
        commands.append(f"cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -nostartfiles -shared -I/source/src -I/source/tests -DFIXTURE_VERSION={version} -DFIXTURE_ABI={abi} /source/tests/helper_loader_fixture.c -ldl -o /results/helper-{version}.so")
    commands.append(" ".join([*compiler, "-I/source/tests", "/source/tests/helper_loader_test.cpp", "/results/checksum.o",
                              "/results/version_store.o", "/results/helper_loader.o", "-ldl", "-pthread", "-o", "/results/helper-loader-test"]))
    commands.extend(("/results/version-store-test", "/results/update-worker-test /results/worker-fixture",
                     "/results/update-bundle-test /results/worker-fixture",
                     "/results/helper-loader-test /results/helper-1.so /results/helper-2.so /results/helper-3.so"))
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
        counts = {name: int(count) for count, name in re.findall(r"PASS: (\d+) (version-store|update-worker|update-bundle|helper-loader)", text)}
        report["checks_by_suite"] = counts
        report["checks"] = sum(counts.values())
        if not report["passed"] or set(counts) != {"version-store", "update-worker", "update-bundle", "helper-loader"}:
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

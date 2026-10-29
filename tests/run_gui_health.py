import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

from sd_ui_test import compact_capture

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description="Check actual manager startup confirmation with an ephemeral in-memory signing key")
    parser.add_argument("--arch", choices=("armv7", "aarch64"), default="aarch64")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Use a fresh output directory inside build/")
    output.mkdir(parents=True, exist_ok=False)
    directory = "/work/build/aarch64" if args.arch == "aarch64" else "/work/build"
    image = "mpc-usb-plugin-installer-build-tools" + (":aarch64" if args.arch == "aarch64" else "")
    container = "pmu-gui-health-" + uuid.uuid4().hex[:12]
    files = [*ROOT.glob("src/*.hpp"), *ROOT.glob("src/*.cpp"), ROOT / "tests/gui_health_test.cpp", ROOT / "tests/signing_fixture.hpp"]
    inputs = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    command = ["docker", "run", "--rm", "--pull=never", "--name", container,
               "--platform=" + ("linux/arm64" if args.arch == "aarch64" else "linux/arm/v7"),
               "--network=none", "--read-only", "--memory=768m", "--pids-limit=128",
               "--tmpfs=/tmp:rw,exec,size=134217728", "--mount", f"type=bind,src={ROOT},dst=/work,readonly",
               "--mount", f"type=bind,src={output},dst=/results", "--workdir=/work", image, "sh", "-ec",
               f"c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Itests tests/gui_health_test.cpp src/checksum.cpp src/crypto.cpp src/version_store.cpp src/source_runtime.cpp -ldl -lcrypto -pthread -o /tmp/gui-health-test; /tmp/gui-health-test {directory} /results"]
    report = {"date": datetime.now().strftime("%d-%m-%Y"), "architecture": args.arch,
              "scope": "Actual SDL application in isolated offline fixtures; no hardware or release-key use", "inputs": inputs}
    try:
        with (output / "tests.log").open("x") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=240)
        text = (output / "tests.log").read_text()
        report.update(returncode=result.returncode, passed=result.returncode == 0 and not re.search(r"warning:|runtime error:|AddressSanitizer:|^FAIL:", text, re.M))
        if not report["passed"]:
            raise RuntimeError("GUI startup verification failed; inspect " + str(output / "tests.log"))
        report["capture"] = compact_capture(output / "first-frame.bmp").name
        print(next(line for line in text.splitlines() if line.startswith("PASS:")))
    finally:
        subprocess.run(["docker", "rm", "-f", container], capture_output=True, timeout=15)
        report["sources_unchanged"] = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == value for name, value in inputs.items())
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()

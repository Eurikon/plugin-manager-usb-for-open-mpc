import argparse
import os
import platform
import signal
import subprocess
import uuid
from pathlib import Path


DEFAULT_IMAGE = "mpc-usb-plugin-installer-build-tools"


def disposable_root(path):
    root = Path(path).resolve()
    temporary = Path("/tmp").resolve()
    if root == temporary or not root.is_relative_to(temporary):
        raise ValueError("Tests require a disposable extracted firmware root under /tmp")
    busybox = root / "usr/bin/busybox.nosuid"
    if not busybox.is_file() or not busybox.resolve().is_relative_to(root):
        raise ValueError("Extracted firmware root must contain usr/bin/busybox.nosuid")
    for relative in ("tmp", "proc/mounts", "sdcard/Expansions", "run", "media/az01-internal/Settings/MPC/MPC.settings"):
        if not (root / relative).resolve().is_relative_to(root):
            raise ValueError(f"Fixture path escapes disposable root: {relative}")
    return root


def add_runtime_arguments(parser):
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("--direct", action="store_true", help="Run chroot as root inside Linux, without Docker")
    parser.add_argument("--timeout", type=int, default=180, help="Maximum seconds per subprocess")


def lifecycle_arguments(*, build_directory=False):
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=disposable_root)
    if build_directory:
        parser.add_argument("--build-directory", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    add_runtime_arguments(parser)
    return parser.parse_args()


def execute(command, timeout=180):
    if timeout <= 0:
        raise ValueError("Timeout must be positive")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        if command[0] == "docker" and "--name" in command:
            name = command[command.index("--name") + 1]
            subprocess.run(["docker", "rm", "-f", name], capture_output=True, timeout=15)
        raise subprocess.TimeoutExpired(command, timeout, output=stdout, stderr=stderr)
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


class ChrootRunner:
    def __init__(self, root, image=DEFAULT_IMAGE, direct=False, timeout=180):
        self.root = disposable_root(root)
        self.image, self.direct, self.timeout = image, direct, timeout
        if direct and (platform.system() != "Linux" or os.geteuid() != 0):
            raise ValueError("--direct requires root inside Linux")
        if timeout <= 0:
            raise ValueError("Timeout must be positive")

    def command(self, script):
        inner = ["chroot", str(self.root), "/usr/bin/busybox.nosuid", "sh", "-c", script]
        if self.direct:
            return inner
        inner[1] = "/firmware-root"
        return ["docker", "run", "--rm", "--pull", "never", "--name", "mpc-manager-test-" + uuid.uuid4().hex,
                "--platform", "linux/arm/v7", "--network", "none", "--mount",
                f"type=bind,src={self.root},dst=/firmware-root", "--entrypoint", "chroot", self.image,
                *inner[1:]]

    def run(self, script):
        return execute(self.command(script), self.timeout)

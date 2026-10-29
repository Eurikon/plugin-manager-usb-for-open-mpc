from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile


def run(*arguments: str) -> bytes:
    result = subprocess.run([os.environ.get("OPENSSL", "openssl"), *arguments],
                            capture_output=True, timeout=30)
    if result.returncode:
        raise ValueError("OpenSSL signing operation failed: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def public_key(private: Path) -> str:
    status = private.lstat()
    if (not stat.S_ISREG(status.st_mode) or status.st_uid != os.geteuid()
            or status.st_mode & 0o077 or status.st_nlink != 1 or status.st_size > 4096):
        raise ValueError("Release key must be a private, owner-only regular file")
    encoded = run("pkey", "-in", str(private), "-pubout", "-outform", "DER")
    if len(encoded) != 44 or encoded[:12] != bytes.fromhex("302a300506032b6570032100"):
        raise ValueError("Release key must use Ed25519")
    return encoded[12:].hex()


def public_header(value: str) -> str:
    return '#pragma once\nnamespace pmu { inline constexpr char publisherKey[] = "' + value + '"; }\n'


def read_public_key(public: Path) -> str:
    match = re.fullmatch(r'#pragma once\nnamespace pmu \{ inline constexpr char publisherKey\[\] = "([0-9a-f]{64})"; \}\n', public.read_text())
    if match is None:
        raise ValueError("Invalid publisher public-key header")
    return match.group(1)


def initialize(private: Path, public: Path) -> None:
    if private.exists() or private.is_symlink() or public.exists() or public.is_symlink():
        raise FileExistsError("Refusing to overwrite an existing release key")
    if not run("version").startswith(b"OpenSSL 3."):
        raise ValueError("Use OpenSSL 3, available in the existing ARM64 build image")
    private.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    owner = private.parent.lstat()
    if not stat.S_ISDIR(owner.st_mode) or owner.st_uid != os.geteuid() or owner.st_mode & 0o077:
        raise ValueError("Release-key directory must be private and owned by the signer")
    encoded = run("genpkey", "-algorithm", "ED25519")
    descriptor = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(encoded)
        output.flush()
        os.fsync(output.fileno())
    value = public_key(private)
    public.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(public, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(descriptor, "w") as output:
        output.write(public_header(value))
        output.flush()
        os.fsync(output.fileno())


class Signer:
    def __init__(self, private: Path, public: Path):
        self.private = private
        expected = read_public_key(public)
        if public_key(private) != expected:
            raise ValueError("Release key does not match the compiled publisher public key")

    def authorize(self, subject: str, kind: str) -> str:
        if kind not in ("runtime", "helper", "update") or not re.fullmatch(r"[0-9a-f]{64}", subject):
            raise ValueError("Invalid release authorization request")
        message = json.dumps({"schema": 1, "product": "plugin-manager-usb", "kind": kind,
                              "generation": subject}, sort_keys=True, separators=(",", ":")) + "\n"
        with tempfile.TemporaryDirectory(prefix="pmu-sign-") as temporary:
            path = Path(temporary) / "message"
            path.write_text(message)
            signature = run("pkeyutl", "-sign", "-rawin", "-inkey", str(self.private), "-in", str(path))
        if len(signature) != 64:
            raise ValueError("Unexpected release signature size")
        return json.dumps({"message": message, "signature": signature.hex()},
                          sort_keys=True, separators=(",", ":")) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare the private publisher key or sign a PMU generation")
    parser.add_argument("--private", required=True, type=Path)
    parser.add_argument("--public", required=True, type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("initialize")
    sign = commands.add_parser("authorize")
    sign.add_argument("kind", choices=("runtime", "helper", "update"))
    sign.add_argument("manifest", type=Path)
    sign.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.command == "initialize":
        initialize(args.private, args.public)
        print("Created the private release key and its public verification key")
    else:
        signer = Signer(args.private, args.public)
        args.output.write_text(signer.authorize(hashlib.sha256(args.manifest.read_bytes()).hexdigest(), args.kind))
        print("Wrote publisher authorization")


if __name__ == "__main__":
    main()

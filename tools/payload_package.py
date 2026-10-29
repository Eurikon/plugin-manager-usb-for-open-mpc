from __future__ import annotations

import hashlib
import json
from pathlib import PurePosixPath
import re


def encode(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode()


def path_name(name: str) -> bool:
    path = PurePosixPath(name)
    return (0 < len(name) <= 240 and not path.is_absolute() and str(path) == name
            and all(part not in (".", "..") and re.fullmatch(r"[A-Za-z0-9_. -]+", part)
                    for part in name.split("/")))


def generation(files: dict[str, bytes], roles: dict[str, str], version: str,
               architecture: str, kind: str, signer) -> tuple[str, dict, dict[str, bytes]]:
    if (architecture not in ("armv7", "aarch64", "universal")
            or not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", version)
            or not 0 < len(files) <= 256 or len(roles) > 32
            or sum(map(len, files.values())) > 128 * 1024 * 1024):
        raise ValueError("Invalid generation metadata or size")
    for name, content in files.items():
        if (not path_name(name) or name.split("/")[0] in ("generation.json", "authorization.json")
                or len(content) > 64 * 1024 * 1024
                or any(str(parent) in files for parent in PurePosixPath(name).parents if str(parent) != ".")):
            raise ValueError("Invalid generation payload path or size")
    if any(not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", role) or name not in files
           for role, name in roles.items()):
        raise ValueError("Generation role is not covered by its inventory")
    specification = {"schema": 1, "version": version, "architecture": architecture,
                     "files": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
                     "entrypoints": roles}
    manifest = encode(specification)
    if len(manifest) > 128 * 1024:
        raise ValueError("Generation inventory exceeds its size limit")
    identity = hashlib.sha256(manifest).hexdigest()
    proof = signer.authorize(identity, kind).encode()
    return identity, specification, {**files, "generation.json": manifest, "authorization.json": proof}


def store(identity: str, payload: dict[str, bytes], profiles: tuple[str, ...] = ("",)) -> dict[str, bytes]:
    if (not re.fullmatch(r"[0-9a-f]{64}", identity) or not profiles
            or len(set(profiles)) != len(profiles)
            or any(profile not in ("", "armv7", "aarch64") for profile in profiles)
            or hashlib.sha256(payload["generation.json"]).hexdigest() != identity):
        raise ValueError("Invalid factory generation selection")
    files = {".pmu-versions/" + identity + "/" + name: data for name, data in payload.items()}
    selection = encode({"schema": 1, "active": identity, "previous": "", "pending": False, "attempted": False})
    for profile in profiles:
        suffix = "." + profile if profile else ""
        for state in ("active", "healthy"):
            files[".pmu-versions/" + state + suffix + ".json"] = selection
    return files


def envelope(files: dict[str, bytes], roles: dict[str, str], version: str,
             architecture: str, signer) -> bytes:
    _, specification, payload = generation(files, roles, version, architecture, "update", signer)
    return encode({"protocol": 1, "product": "plugin-manager-usb", "generation": specification,
                   "authorization": payload["authorization.json"].decode()})

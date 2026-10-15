"""Locate a consistent autorun salt without assuming its firmware offset."""

import hashlib
import hmac
import lzma
import os
import re
import stat
import zlib
from pathlib import Path

KEY = (0xC9771B45F9913B87).to_bytes(8, "little")
WIDTH = 37
MIN_LEN = 16
CHUNK = 1024 * 1024
MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_STREAM_BYTES = 512 * 1024 * 1024
MAX_SCANNED_BYTES = 1024 * 1024 * 1024
MAX_XZ_MEMORY = 256 * 1024 * 1024
MAX_XZ_OFFSETS = 64
MAX_CANDIDATES = 1024
XZ_MAGIC = b"\xfd7zXZ\x00"
_RUN = re.compile(rb"[\x20-\x7e]{16,}")
_XOR_TABLES = tuple(bytes(value ^ key for value in range(256)) for key in KEY)


class _Budget:
    def __init__(self):
        self.used = 0

    def add(self, count):
        self.used += count
        if self.used > MAX_SCANNED_BYTES:
            raise ValueError("Firmware scan exceeds the cumulative byte limit")


def _xor_for(block, start, align):
    decoded = bytearray(len(block))
    for lane in range(8):
        decoded[lane::8] = block[lane::8].translate(_XOR_TABLES[(start + lane - align) % 8])
    return decoded


def scan(stream):
    """Yield complete (stream offset, salt bytes) candidates within fixed limits."""
    base, tail, candidates, deferred = 0, b"", 0, set()
    while True:
        request = min(CHUNK, MAX_STREAM_BYTES - base + 1)
        data = stream.read(request)
        if not isinstance(data, bytes) or len(data) > request:
            raise ValueError("Firmware reader violated the bounded binary read contract")
        final = not data
        if final and not deferred:
            return
        if base + len(data) > MAX_STREAM_BYTES:
            raise ValueError("Firmware stream exceeds the scan byte limit")
        block, start = tail + data, base - len(tail)
        next_deferred = set()
        for align in range(8):
            decoded = _xor_for(block, start, align)
            for run in _RUN.finditer(decoded):
                first, end = run.span()
                lower = max(first, end - WIDTH)
                candidate = lower + ((align - start - lower) % 8)
                while candidate <= end - MIN_LEN:
                    stop = candidate + WIDTH
                    offset = start + candidate
                    if end == len(block) and stop == end and not final:
                        next_deferred.add(offset)
                    elif stop <= len(block) and (start + stop > base or offset in deferred) and not any(decoded[end:stop]):
                        candidates += 1
                        if candidates > MAX_CANDIDATES:
                            raise ValueError("Firmware contains too many salt candidates")
                        yield offset, bytes(decoded[candidate:end])
                    candidate += 8
        if final:
            return
        base += len(data)
        tail, deferred = block[-(WIDTH + 8):], next_deferred


class _RawReader:
    def __init__(self, source, budget):
        self.source, self.budget = source, budget
        self.offsets, self.position, self.tail = [], 0, b""

    def read(self, count):
        data = self.source.read(count)
        self.budget.add(len(data))
        block, start = self.tail + data, self.position - len(self.tail)
        index = block.find(XZ_MAGIC)
        while index >= 0:
            offset = start + index
            if offset + len(XZ_MAGIC) > self.position:
                self.offsets.append(offset)
                if len(self.offsets) > MAX_XZ_OFFSETS:
                    raise ValueError("Firmware contains too many compressed-stream markers")
            index = block.find(XZ_MAGIC, index + 1)
        self.position += len(data)
        self.tail = block[-(len(XZ_MAGIC) - 1):]
        return data


class _Xz:
    def __init__(self, source, offset, budget=None):
        source.seek(offset)
        self.source = source
        self.budget = _Budget() if budget is None else budget
        self.decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=MAX_XZ_MEMORY)
        self.total = 0

    def read(self, count):
        if not isinstance(count, int) or not 0 <= count <= CHUNK:
            raise ValueError("Invalid compressed-stream read size")
        output = bytearray()
        while len(output) < count and not self.decoder.eof:
            raw = self.source.read(65536) if self.decoder.needs_input else b""
            if self.decoder.needs_input and not raw:
                raise ValueError("Firmware compressed stream is truncated")
            self.budget.add(len(raw))
            decoded = self.decoder.decompress(raw, max_length=min(count - len(output),
                                                                  MAX_STREAM_BYTES - self.total + 1))
            self.total += len(decoded)
            self.budget.add(len(decoded))
            if self.total > MAX_STREAM_BYTES:
                raise ValueError("Firmware decompressed stream exceeds the scan byte limit")
            if not raw and not decoded and not self.decoder.eof and not self.decoder.needs_input:
                raise ValueError("Firmware compressed stream made no progress")
            output.extend(decoded)
        return bytes(output)


def _xz_header(source, offset, budget):
    source.seek(offset)
    header = source.read(12)
    budget.add(len(header))
    return (len(header) == 12 and header[:6] == XZ_MAGIC and header[6] == 0 and
            header[7] in (0, 1, 4, 10) and
            zlib.crc32(header[6:8]).to_bytes(4, "little") == header[8:12])


def find_salt(path, expected_sha256=None):
    """Return (salt, locations); reject absent, conflicting or unexpected values."""
    if expected_sha256 is not None:
        if not isinstance(expected_sha256, str) or re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha256) is None:
            raise ValueError("Expected salt fingerprint must be a SHA-256 hex digest")
        expected_sha256 = expected_sha256.lower()
    budget, hits = _Budget(), []

    def collect(where, found):
        pending = [(where, offset, value) for offset, value in found]
        if len(hits) + len(pending) > MAX_CANDIDATES:
            raise ValueError("Firmware contains too many salt candidates")
        hits.extend(pending)

    descriptor = os.open(Path(path), os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > MAX_FILE_BYTES:
            raise ValueError("Firmware must be a regular file within the byte limit")
        raw = _RawReader(source, budget)
        collect("file", scan(raw))
        for offset in raw.offsets:
            if not _xz_header(source, offset, budget):
                continue
            try:
                collect(f"xz@{offset}", scan(_Xz(source, offset, budget)))
            except lzma.LZMAError as error:
                raise ValueError("Firmware compressed stream is invalid or exceeds its memory limit") from error
        after = os.fstat(source.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("Firmware changed while it was being scanned")
    if not hits:
        raise ValueError("No complete autorun salt candidate found")
    salt = hits[0][2]
    if any(value != salt for _, _, value in hits):
        raise ValueError("Conflicting autorun salt candidates; refusing to guess")
    if expected_sha256 is not None and not hmac.compare_digest(hashlib.sha256(salt).hexdigest(), expected_sha256):
        raise ValueError("Unexpected autorun salt; refusing firmware")
    return salt, [(where, offset) for where, offset, _ in hits]

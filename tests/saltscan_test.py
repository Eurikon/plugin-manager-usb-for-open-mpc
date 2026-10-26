import hashlib
import importlib.util
import io
import lzma
import os
from pathlib import Path
import random
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("manager_saltscan",
                                             Path(__file__).resolve().parents[1] / "tools/saltscan.py")
SCANNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCANNER)
SALT = b"fixture-salt-123"
OTHER = b"fixture-salt-456"


def encode(data, key_start=0):
    return bytes(value ^ SCANNER.KEY[(index + key_start) % 8] for index, value in enumerate(data))


def raw_fixture(salt=SALT, offset=32, trailing=32):
    return b"\xff" * offset + encode(salt + bytes(SCANNER.WIDTH - len(salt))) + b"\xff" * trailing


def reference_candidates(data):
    found = []
    for offset in range(len(data) - 36):
        decoded = bytes(byte ^ SCANNER.KEY[index % 8]
                        for index, byte in enumerate(data[offset:offset + 38]))
        window = decoded[:37]
        printed = 0
        while printed < 37 and 32 <= window[printed] <= 126:
            printed += 1
        continued = printed == 37 and len(decoded) == 38 and 32 <= decoded[37] <= 126
        if printed >= 16 and not continued and window[printed:] == bytes(37 - printed):
            found.append((offset, window[:printed]))
    return found


class SaltScanTests(unittest.TestCase):
    def find(self, data, **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "inert firmware fixture"
            source.write_bytes(data)
            before = source.stat().st_mtime_ns
            result = SCANNER.find_salt(source, **kwargs)
            self.assertEqual(source.read_bytes(), data)
            self.assertEqual(source.stat().st_mtime_ns, before)
            return result

    def test_all_key_alignments_and_salt_lengths(self):
        for alignment in range(8):
            for length in (16, 17, 24, 36, 37):
                salt = bytes(65 + index % 26 for index in range(length))
                offset = 32 + alignment
                with self.subTest(alignment=alignment, length=length):
                    self.assertEqual(self.find(raw_fixture(salt, offset)), (salt, [("file", offset)]))

    def test_complete_window_at_eof_and_truncated_windows(self):
        self.assertEqual(self.find(raw_fixture(trailing=0)), (SALT, [("file", 32)]))
        for length in range(len(SALT), SCANNER.WIDTH):
            with self.subTest(length=length):
                self.assertEqual(list(SCANNER.scan(io.BytesIO(raw_fixture(trailing=0)[:32 + length]))), [])

    def test_nonprintable_empty_short_and_nonzero_padding_are_rejected(self):
        decoded = [bytes(37), b"a" * 15 + bytes(22), b"a" * 16 + b"\x01" + bytes(20),
                   b"a" * 15 + b"\x7f" + bytes(21), b"a" * 15 + b"\x1f" + bytes(21)]
        for value in decoded:
            with self.subTest(value=value[:17]):
                self.assertEqual(list(SCANNER.scan(io.BytesIO(encode(value)))), [])

    def test_printable_space_edges_are_kept(self):
        salt = b" " + SALT[:-2] + b" "
        self.assertEqual(self.find(raw_fixture(salt)), (salt, [("file", 32)]))

    def test_single_adjacent_printable_byte_does_not_change_salt(self):
        data = bytearray(raw_fixture())
        data[31] = ord("Q") ^ SCANNER.KEY[-1]
        self.assertEqual(self.find(bytes(data)), (SALT, [("file", 32)]))

    def test_short_adjacent_printable_prefixes_keep_the_first_aligned_start(self):
        for length in (16, 37):
            salt = b"Q" * length
            for prefix in range(8):
                offset = 32 + prefix
                background = b"\xff" * 31 + encode(b"\x01", key_start=(-prefix - 1) % 8)
                content = background + encode(b"R" * prefix, key_start=(-prefix) % 8)
                content += encode(salt + bytes(37 - length))
                for chunk in (1, 17, 37, 64):
                    with self.subTest(length=length, prefix=prefix, chunk=chunk), \
                            patch.object(SCANNER, "CHUNK", chunk):
                        self.assertEqual(list(SCANNER.scan(io.BytesIO(content))), [(offset, salt)])

    def test_ambiguous_printable_prefix_is_not_guessed(self):
        data = bytearray(raw_fixture())
        data[12:32] = encode(b"Q" * 20, key_start=4)
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(bytes(data), expected_sha256=hashlib.sha256(SALT).hexdigest())

    def test_two_distinct_candidates_are_not_filtered_by_known_fingerprint(self):
        content = b"\xff" * 31 + encode(b"\x01", key_start=7)
        content += encode(b"Q" * 8) + encode(SALT + bytes(21)) + b"\xff" * 32
        self.assertEqual(list(SCANNER.scan(io.BytesIO(content))), [(32, b"Q" * 8 + SALT), (40, SALT)])
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(content, expected_sha256=hashlib.sha256(SALT).hexdigest())

    def test_all_valid_aligned_windows_are_considered(self):
        salt = b"A" * 37
        data = encode(salt + bytes(16))
        found = list(SCANNER.scan(io.BytesIO(data)))
        self.assertEqual(found, [(0, salt), (8, b"A" * 29), (16, b"A" * 21)])
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(data)

    def test_chunk_boundaries_and_deduplication(self):
        for chunk in (8, 19, 37, 64):
            for offset in range(0, 2 * chunk):
                with self.subTest(chunk=chunk, offset=offset), patch.object(SCANNER, "CHUNK", chunk):
                    self.assertEqual(self.find(raw_fixture(offset=offset)), (SALT, [("file", offset)]))

    def test_duplicate_candidates_with_identical_values_are_allowed(self):
        data = raw_fixture() + raw_fixture(offset=5)
        self.assertEqual(self.find(data), (SALT, [("file", 32), ("file", len(raw_fixture()) + 5)]))

    def test_different_candidates_are_rejected_without_value_disclosure(self):
        with self.assertRaisesRegex(ValueError, "Conflicting") as caught:
            self.find(raw_fixture() + raw_fixture(OTHER))
        self.assertNotIn(SALT.decode(), str(caught.exception))
        self.assertNotIn(OTHER.decode(), str(caught.exception))

    def test_fingerprint_acceptance_and_rejection(self):
        fingerprint = hashlib.sha256(SALT).hexdigest()
        self.assertEqual(self.find(raw_fixture(), expected_sha256=fingerprint.upper())[0], SALT)
        with self.assertRaisesRegex(ValueError, "Unexpected") as caught:
            self.find(raw_fixture(), expected_sha256="0" * 64)
        self.assertNotIn(SALT.decode(), str(caught.exception))
        self.assertNotIn(fingerprint, str(caught.exception))
        for value in ("", "f" * 63, "g" * 64, b"f" * 64, 1):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "fingerprint"):
                self.find(raw_fixture(), expected_sha256=value)

    def test_no_candidates_are_rejected(self):
        for data in (b"", b"plain inert bytes", b"\xff" * 1000):
            with self.subTest(length=len(data)), self.assertRaisesRegex(ValueError, "No complete"):
                self.find(data)

    def test_binary_reader_contract_is_enforced(self):
        class BadReader:
            def __init__(self, result):
                self.result = result

            def read(self, count):
                return self.result if self.result != "oversized" else bytes(count + 1)

        for value in (None, "text", bytearray(), "oversized"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "read contract"):
                list(SCANNER.scan(BadReader(value)))

    def test_window_scanner_matches_independent_bytewise_reference(self):
        random_source = random.Random(731)
        for length in (16, 25, 37):
            for chunk in (17, 53, 128):
                data = bytearray(random_source.randbytes(350))
                value = random_source.randbytes(length).hex()[:length].encode()
                data[119:156] = encode(value + bytes(37 - length))
                with self.subTest(length=length, chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                    self.assertEqual(sorted(SCANNER.scan(io.BytesIO(bytes(data)))), reference_candidates(data))

    def test_fragmented_source_reads_preserve_windows(self):
        class Fragmented(io.BytesIO):
            def read(self, count):
                return super().read(min(count, 3))

        self.assertEqual(list(SCANNER.scan(Fragmented(raw_fixture()))), [(32, SALT)])

    def test_long_printable_runs_are_not_truncated_into_candidates(self):
        for length in (38, 40, 47, 64, 100):
            content = encode(b"Q" * length + b"\x01" * 37)
            for chunk in (19, 37, 40, 64, 128):
                with self.subTest(length=length, chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                    self.assertEqual(list(SCANNER.scan(io.BytesIO(content))), [])

    def test_full_width_candidate_requires_termination_or_verified_eof(self):
        value = b"Q" * 37
        for tail in (b"", b"\0", b"\x01"):
            for chunk in (1, 8, 19, 37, 38, 74):
                with self.subTest(tail=tail, chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                    self.assertEqual(list(SCANNER.scan(io.BytesIO(encode(value + tail)))), [(0, value)])
        for chunk in (1, 8, 19, 37, 38, 74):
            with self.subTest(chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                self.assertEqual(list(SCANNER.scan(io.BytesIO(encode(value + b"Q" + b"\x01" * 37)))), [])

    def test_full_width_boundary_candidates_are_not_duplicated(self):
        value = b"Q" * 37
        for offset in range(0, 75):
            for chunk in (19, 37, 64):
                with self.subTest(offset=offset, chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                    self.assertEqual(list(SCANNER.scan(io.BytesIO(raw_fixture(value, offset=offset)))), [(offset, value)])

    def test_printable_extension_with_padded_suffixes_is_ambiguous(self):
        value = b"Q" * 37
        content = encode(value + b"Q" + bytes(37))
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(content, expected_sha256=hashlib.sha256(value).hexdigest())

    def test_structural_suffix_candidate_requires_trusted_fingerprint(self):
        content = encode(b"A" * 37 + b"B" + bytes(7))
        expected = [(8, b"A" * 29 + b"B")]
        for chunk in (1, 8, 19, 37, 38, 45, 64):
            with self.subTest(chunk=chunk), patch.object(SCANNER, "CHUNK", chunk):
                self.assertEqual(list(SCANNER.scan(io.BytesIO(content))), expected)
                with self.assertRaisesRegex(ValueError, "Unexpected"):
                    self.find(content, expected_sha256=hashlib.sha256(SALT).hexdigest())

    def test_long_run_candidates_are_stable_across_every_chunk_alignment(self):
        for length in (38, 45, 64, 100):
            for offset in range(0, 72):
                content = b"\xff" * offset + encode(b"A" * length + bytes(37))
                expected = reference_candidates(content)
                for chunk in (17, 37, 64):
                    with self.subTest(length=length, offset=offset, chunk=chunk), \
                            patch.object(SCANNER, "CHUNK", chunk):
                        self.assertEqual(sorted(SCANNER.scan(io.BytesIO(content))), expected)

    def test_long_printable_prefixes_do_not_hide_valid_windows(self):
        salt = b"Q" * 36
        for alignment in range(8):
            for prefix in (8, 16, 24, 31):
                start = 32 + alignment
                offset = start + prefix
                content = b"\xff" * start + encode(b"R" * prefix, key_start=(-prefix) % 8)
                content += encode(salt + b"\0") + b"\xff" * 37
                self.assertEqual(self.find(content, expected_sha256=hashlib.sha256(salt).hexdigest()),
                                 (salt, [("file", offset)]))
                for chunk in (1, 17, 37, 64):
                    with self.subTest(alignment=alignment, prefix=prefix, chunk=chunk), \
                            patch.object(SCANNER, "CHUNK", chunk):
                        self.assertEqual(list(SCANNER.scan(io.BytesIO(content))), [(offset, salt)])

    def test_raw_file_and_stream_limits(self):
        with patch.object(SCANNER, "MAX_FILE_BYTES", 50), self.assertRaisesRegex(ValueError, "regular file"):
            self.find(raw_fixture())
        with patch.object(SCANNER, "MAX_STREAM_BYTES", 50), self.assertRaisesRegex(ValueError, "stream exceeds"):
            list(SCANNER.scan(io.BytesIO(raw_fixture())))
        with patch.object(SCANNER, "MAX_STREAM_BYTES", 37):
            self.assertEqual(list(SCANNER.scan(io.BytesIO(encode(SALT + bytes(21))))), [(0, SALT)])

    def test_total_byte_budget_is_enforced(self):
        with patch.object(SCANNER, "MAX_SCANNED_BYTES", 50), self.assertRaisesRegex(ValueError, "cumulative"):
            self.find(raw_fixture())

    def test_nonregular_files_do_not_block_or_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fifo = root / "fifo"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, "regular file"):
                SCANNER.find_salt(fifo)
            with self.assertRaises((ValueError, IsADirectoryError)):
                SCANNER.find_salt(root)

    def test_changed_file_is_rejected(self):
        original = SCANNER.os.fstat
        checks = 0

        def changed(descriptor):
            nonlocal checks
            state = original(descriptor)
            checks += 1
            return SimpleNamespace(st_mode=state.st_mode, st_size=state.st_size,
                                   st_mtime_ns=state.st_mtime_ns + checks,
                                   st_ctime_ns=state.st_ctime_ns)

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "inert fixture"
            source.write_bytes(raw_fixture())
            with patch.object(SCANNER.os, "fstat", side_effect=changed), \
                    self.assertRaisesRegex(ValueError, "changed"):
                SCANNER.find_salt(source)

    def test_candidate_limit_is_enforced_across_streams(self):
        with patch.object(SCANNER, "MAX_CANDIDATES", 1), self.assertRaisesRegex(ValueError, "too many salt"):
            self.find(raw_fixture() + raw_fixture())
        with patch.object(SCANNER, "MAX_CANDIDATES", 1), self.assertRaisesRegex(ValueError, "too many salt"):
            self.find(raw_fixture() + lzma.compress(raw_fixture()))

    def test_compressed_root_and_embedded_streams(self):
        compressed = lzma.compress(raw_fixture())
        for prefix in (b"", b"AZ01inert fixture", b"unstructured prefix"):
            with self.subTest(prefix=prefix):
                self.assertEqual(self.find(prefix + compressed), (SALT, [(f"xz@{len(prefix)}", 32)]))

    def test_identical_raw_and_compressed_candidates_are_kept(self):
        raw = raw_fixture()
        self.assertEqual(self.find(raw + lzma.compress(raw)),
                         (SALT, [("file", 32), (f"xz@{len(raw)}", 32)]))

    def test_raw_candidate_does_not_suppress_compressed_conflict(self):
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(raw_fixture() + lzma.compress(raw_fixture(OTHER)))

    def test_multiple_compressed_streams_must_agree(self):
        first, second = lzma.compress(raw_fixture()), lzma.compress(raw_fixture())
        self.assertEqual(self.find(first + second), (SALT, [("xz@0", 32), (f"xz@{len(first)}", 32)]))
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.find(first + lzma.compress(raw_fixture(OTHER)))

    def test_invalid_magic_inside_raw_binary_is_ignored(self):
        for suffix in (SCANNER.XZ_MAGIC, SCANNER.XZ_MAGIC + bytes(100),
                       SCANNER.XZ_MAGIC + b"\x00\x01" + bytes(4)):
            with self.subTest(length=len(suffix)):
                self.assertEqual(self.find(raw_fixture() + suffix), (SALT, [("file", 32)]))

    def test_stream_marker_boundaries_and_limit(self):
        compressed = lzma.compress(raw_fixture())
        for offset in range(0, 24):
            with self.subTest(offset=offset), patch.object(SCANNER, "CHUNK", 19):
                self.assertEqual(self.find(b"\xff" * offset + compressed), (SALT, [(f"xz@{offset}", 32)]))
        with patch.object(SCANNER, "MAX_XZ_OFFSETS", 1), self.assertRaisesRegex(ValueError, "too many compressed"):
            self.find(raw_fixture() + SCANNER.XZ_MAGIC * 2)

    def test_truncated_streams_cannot_contribute_candidates(self):
        compressed = lzma.compress(raw_fixture() + bytes(10000))
        for remove in (1, 4, 12, 20):
            for prefix in (b"", raw_fixture()):
                with self.subTest(remove=remove, prefix=len(prefix)), self.assertRaises(ValueError):
                    self.find(prefix + compressed[:-remove])

    def test_corrupt_streams_cannot_contribute_candidates(self):
        compressed = bytearray(lzma.compress(raw_fixture() + bytes(10000)))
        for offset in (20, len(compressed) - 8):
            data = compressed.copy()
            data[offset] ^= 0x40
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.find(raw_fixture() + bytes(data))

    def test_xz_reads_respect_request_size_and_reach_verified_eof(self):
        content = bytes(1024 * 1024)
        reader = SCANNER._Xz(io.BytesIO(lzma.compress(content)), 0)
        self.assertEqual(reader.read(64), bytes(64))
        self.assertEqual(reader.total, 64)
        self.assertFalse(reader.decoder.eof)
        result = reader.read(SCANNER.CHUNK)
        self.assertEqual(result, content[64:])
        self.assertTrue(reader.decoder.eof)
        self.assertEqual(reader.read(64), b"")
        for value in (-1, SCANNER.CHUNK + 1, None, "64"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "read size"):
                reader.read(value)

    def test_decompression_memory_and_output_limits(self):
        compressed = lzma.compress(raw_fixture() + bytes(100000))
        with patch.object(SCANNER, "MAX_XZ_MEMORY", 1024), self.assertRaisesRegex(ValueError, "memory limit"):
            self.find(compressed)
        with patch.object(SCANNER, "MAX_STREAM_BYTES", 1024), self.assertRaisesRegex(ValueError, "decompressed"):
            self.find(compressed)
        with patch.object(SCANNER, "MAX_SCANNED_BYTES", len(compressed) + 100), \
                self.assertRaisesRegex(ValueError, "cumulative"):
            self.find(compressed)

    def test_decompressor_is_always_given_a_bounded_output_request(self):
        original = SCANNER.lzma.LZMADecompressor
        requests = []

        class ObservedDecoder:
            def __init__(self, *args, **kwargs):
                self.decoder = original(*args, **kwargs)
                self.memory_limit = kwargs["memlimit"]

            @property
            def eof(self):
                return self.decoder.eof

            @property
            def needs_input(self):
                return self.decoder.needs_input

            def decompress(self, data, max_length):
                requests.append(max_length)
                return self.decoder.decompress(data, max_length=max_length)

        with patch.object(SCANNER.lzma, "LZMADecompressor", ObservedDecoder):
            self.assertEqual(self.find(lzma.compress(raw_fixture()))[0], SALT)
        self.assertTrue(requests)
        self.assertTrue(all(0 < count <= SCANNER.CHUNK for count in requests))

    def test_stalled_decoder_fails_without_looping(self):
        class StalledDecoder:
            needs_input = False
            eof = False

            def __init__(self, *args, **kwargs):
                pass

            def decompress(self, data, max_length):
                return b""

        with patch.object(SCANNER.lzma, "LZMADecompressor", StalledDecoder), \
                self.assertRaisesRegex(ValueError, "no progress"):
            SCANNER._Xz(io.BytesIO(b"inert input"), 0).read(64)


if __name__ == "__main__":
    unittest.main()

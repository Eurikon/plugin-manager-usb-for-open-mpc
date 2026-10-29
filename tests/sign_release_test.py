import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_signing", ROOT / "tools/sign_release.py")
signing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing)


class SigningTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pmu-signing-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.private = self.root / "key.pem"
        self.public = self.root / "publisher_key.hpp"
        signing.initialize(self.private, self.public)

    def test_key_permissions_and_refused_overwrite(self):
        self.assertEqual(self.private.stat().st_mode & 0o777, 0o600)
        self.assertRegex(signing.read_public_key(self.public), r"^[0-9a-f]{64}$")
        identity = hashlib.sha256(self.private.read_bytes()).digest()
        with self.assertRaises(FileExistsError):
            signing.initialize(self.private, self.public)
        self.assertEqual(hashlib.sha256(self.private.read_bytes()).digest(), identity)

    def test_signature_interoperates_with_openssl(self):
        subject = hashlib.sha256(b"complete generation").hexdigest()
        proof = json.loads(signing.Signer(self.private, self.public).authorize(subject, "runtime"))
        self.assertEqual(json.loads(proof["message"]), {"schema": 1, "product": "plugin-manager-usb",
                                                       "generation": subject, "kind": "runtime"})
        message, signature, public = [self.root / name for name in ("message", "signature", "public.der")]
        message.write_text(proof["message"])
        signature.write_bytes(bytes.fromhex(proof["signature"]))
        public.write_bytes(bytes.fromhex("302a300506032b6570032100" + signing.read_public_key(self.public)))
        command = [os.environ.get("OPENSSL", "openssl"), "pkeyutl", "-verify", "-pubin", "-inkey",
                   str(public), "-keyform", "DER", "-rawin", "-in", str(message), "-sigfile", str(signature)]
        result = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        message.write_text(proof["message"] + "changed")
        self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)

    def test_mismatched_publisher_is_refused(self):
        self.public.write_text(signing.public_header("0" * 64))
        with self.assertRaisesRegex(ValueError, "does not match"):
            signing.Signer(self.private, self.public)

    def test_exposed_and_linked_private_keys_are_refused(self):
        self.private.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            signing.public_key(self.private)
        self.private.chmod(0o600)
        linked = self.root / "linked.pem"
        linked.symlink_to(self.private)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            signing.public_key(linked)
        linked.unlink()
        os.link(self.private, linked)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            signing.public_key(self.private)

    def test_only_bounded_product_scopes_are_signed(self):
        signer = signing.Signer(self.private, self.public)
        for subject, kind in (("bad", "runtime"), ("0" * 64, "arbitrary-command")):
            with self.assertRaisesRegex(ValueError, "Invalid release authorization"):
                signer.authorize(subject, kind)

    def test_key_creation_refuses_shared_directory(self):
        shared = self.root / "shared"
        shared.mkdir(mode=0o755)
        with self.assertRaisesRegex(ValueError, "directory must be private"):
            signing.initialize(shared / "new.pem", self.root / "new.hex")
        self.assertFalse((shared / "new.pem").exists())


if __name__ == "__main__":
    unittest.main()

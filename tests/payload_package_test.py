import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from tools.payload_package import encode, envelope, generation, path_name, store
from tools.sign_release import Signer, initialize


class PayloadPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pmu-payload-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        initialize(self.root / "key/release.pem", self.root / "publisher.hpp")
        self.signer = Signer(self.root / "key/release.pem", self.root / "publisher.hpp")

    def payload(self):
        return generation({"bin/program": b"native", "assets/My Font": b"font"},
                          {"manager_armv7": "bin/program"}, "2.0.0", "universal", "runtime", self.signer)

    def test_canonical_inventory_and_authorization(self):
        identity, spec, files = self.payload()
        self.assertEqual(hashlib.sha256(files["generation.json"]).hexdigest(), identity)
        self.assertEqual(files["generation.json"], encode(spec))
        proof = json.loads(files["authorization.json"])
        message = json.loads(proof["message"])
        self.assertEqual((message["kind"], message["generation"]), ("runtime", identity))
        self.assertEqual(len(proof["signature"]), 128)

    def test_factory_profiles_have_independent_records(self):
        identity, _, payload = self.payload()
        files = store(identity, payload, ("armv7", "aarch64"))
        for architecture in ("armv7", "aarch64"):
            value = json.loads(files[f".pmu-versions/active.{architecture}.json"])
            self.assertEqual(value["active"], identity)
            self.assertFalse(value["pending"])
        self.assertNotIn(".pmu-versions/active.json", files)

    def test_envelope_binds_future_owned_layout(self):
        value = json.loads(envelope({"future/bin/program": b"worker"},
                                   {"update_worker_armv7": "future/bin/program"},
                                   "4.0.0", "armv7", self.signer))
        self.assertEqual(set(value), {"protocol", "product", "generation", "authorization"})
        self.assertEqual(json.loads(json.loads(value["authorization"])["message"])["kind"], "update")

    def test_unsafe_and_reserved_paths_are_refused(self):
        for name in ("../outside", "/absolute", "a//b", "a/./b", "a/../b", "a\x00b", "a\\b",
                     "generation.json", "authorization.json/child"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                generation({name: b"file"}, {}, "1.0.0", "armv7", "runtime", self.signer)
        self.assertTrue(path_name("valid/Some File.bin"))

    def test_conflicting_files_and_uninventoried_roles_are_refused(self):
        for files, roles in (({"a": b"x", "a/b": b"y"}, {}), ({"a": b"x"}, {"manager": "b"})):
            with self.assertRaises(ValueError):
                generation(files, roles, "1.0.0", "armv7", "runtime", self.signer)

    def test_bad_factory_profiles_and_identity_are_refused(self):
        identity, _, payload = self.payload()
        for identifier, profiles in (("0" * 64, ("",)), (identity, ("../outside",)),
                                     (identity, ("armv7", "armv7"))):
            with self.assertRaises(ValueError):
                store(identifier, payload, profiles)


if __name__ == "__main__":
    unittest.main()

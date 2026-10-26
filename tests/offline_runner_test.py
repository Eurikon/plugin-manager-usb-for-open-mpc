import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from offline_support import ChrootRunner, disposable_root, execute
from prepare_fixtures import catalog_item, package_manifest, version_neighbors
from run_offline import build_command, main, preflight


class OfflineRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir='/tmp', prefix='manager-runner-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / 'root'
        (self.root / 'usr/bin').mkdir(parents=True)
        (self.root / 'usr/bin/busybox.nosuid').write_bytes(b'firmware test marker')

    def supplied_package(self):
        value = os.environ.get('TEST_PACKAGE_ZIP')
        if not value:
            self.skipTest('Set TEST_PACKAGE_ZIP to your own trusted ARMv7 OS 3 package')
        source = Path(value).resolve()
        self.assertTrue(source.is_file(), 'TEST_PACKAGE_ZIP does not exist')
        package = self.base / 'package.zip'
        shutil.copyfile(source, package)
        return package

    def changed_manifest(self, value):
        package = self.supplied_package()
        output = self.base / 'changed.zip'
        with zipfile.ZipFile(package) as source, zipfile.ZipFile(output, 'w') as target:
            for entry in source.infolist():
                data = source.read(entry)
                if entry.filename.count('/') == 1 and entry.filename.endswith('/mpc-plugin.json'):
                    data = json.dumps(value).encode()
                target.writestr(entry, data)
        return output

    def test_preflight_accepts_supplied_release_version(self):
        package = self.supplied_package()
        result = preflight(self.root, package)
        expected = package_manifest(package)
        self.assertEqual(result[3]['id'], expected['id'])
        self.assertEqual(result[3]['version'], expected['version'])
        self.assertIsNone(result[2])
        self.assertIsNone(result[4])

    def test_root_must_be_extracted_under_tmp(self):
        for path in ('/', '/tmp', '/sdcard', self.base / 'missing'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                disposable_root(path)

    def test_fixture_cannot_escape_through_symlink(self):
        (self.root / 'tmp').symlink_to(self.base)
        with self.assertRaisesRegex(ValueError, 'escapes'):
            disposable_root(self.root)

    def test_firmware_binary_cannot_be_outside_root(self):
        external = self.base / 'external-binary'
        external.write_bytes(b'firmware test marker')
        busybox = self.root / 'usr/bin/busybox.nosuid'
        busybox.unlink()
        busybox.symlink_to(external)
        with self.assertRaises(ValueError):
            disposable_root(self.root)

    def test_primary_package_is_mandatory(self):
        with self.assertRaisesRegex(ValueError, 'Missing supplied package'):
            preflight(self.root, self.base / 'absent.zip')

    def test_legacy_fixture_requires_explicit_confirmation(self):
        with self.assertRaisesRegex(ValueError, 'Confirm legacy'):
            preflight(self.root, self.base / 'primary.zip', self.base / 'legacy.zip')
        with self.assertRaisesRegex(ValueError, 'requires --legacy-package'):
            preflight(self.root, self.base / 'primary.zip', legacy_os3_confirmed=True)

    def test_legacy_fixture_requires_missing_compatibility_field(self):
        package = self.supplied_package()
        with self.assertRaisesRegex(ValueError, 'must omit os_compat'):
            preflight(self.root, package, package, legacy_os3_confirmed=True)

    def test_supplied_legacy_fixture_is_optional(self):
        primary = self.supplied_package()
        source = os.environ.get('LEGACY_PACKAGE_ZIP')
        if not source:
            self.skipTest('Set LEGACY_PACKAGE_ZIP for the optional missing-os_compat case')
        result = preflight(self.root, primary, Path(source), legacy_os3_confirmed=True)
        self.assertNotIn('os_compat', result[4])
        self.assertEqual(result[4]['version'], package_manifest(Path(source), legacy=True)['version'])

    def test_fixture_zip_must_be_outside_mutated_root(self):
        package = self.supplied_package()
        inside = self.root / 'package.zip'
        shutil.copyfile(package, inside)
        with self.assertRaisesRegex(ValueError, 'outside'):
            preflight(self.root, inside)

    def test_ambiguous_package_manifest_is_rejected(self):
        package = self.supplied_package()
        with zipfile.ZipFile(package, 'a') as archive:
            archive.writestr('manager-test-other/mpc-plugin.json', '{}')
        with self.assertRaisesRegex(ValueError, 'one package'):
            package_manifest(package)

    def test_missing_version_is_rejected(self):
        package = self.supplied_package()
        value = package_manifest(package)
        value['version'] = ''
        with self.assertRaisesRegex(ValueError, 'package version'):
            package_manifest(self.changed_manifest(value))

    def test_malformed_manifest_shape_is_rejected(self):
        package = self.supplied_package()
        original = package_manifest(package)
        for value in ([], dict(original, version=1)):
            with self.subTest(manifest=value), self.assertRaises(ValueError):
                package_manifest(self.changed_manifest(value))

    def test_changed_manifest_checksum_is_rejected(self):
        package = self.supplied_package()
        value = package_manifest(package)
        value['name'] += ' modified'
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            package_manifest(self.changed_manifest(value))

    def test_unsafe_archive_path_is_rejected_before_checksum_validation(self):
        package = self.supplied_package()
        with zipfile.ZipFile(package, 'a') as archive:
            archive.writestr('../manager-test-escape', 'attack')
        with self.assertRaisesRegex(ValueError, 'Unsafe package path'):
            package_manifest(package)

    def test_catalog_metadata_comes_from_package(self):
        package = self.supplied_package()
        manifest = package_manifest(package)
        item = catalog_item(package, manifest)
        for field in ('id', 'name', 'version', 'uid', 'so', 'skin', 'folder', 'kind', 'max_glibc'):
            self.assertEqual(item[field], manifest[field])
        self.assertEqual(item['author'], manifest.get('author') or manifest.get('manufacturer') or '')
        self.assertEqual(item['url'], 'https://fixture/package.zip')
        self.assertTrue(item['defer'])
        self.assertEqual(int(item['size']), package.stat().st_size)

    def test_numeric_version_neighbors_match_numeric_comparison(self):
        self.assertEqual(version_neighbors('1.2.3'), ('1.2.4', '1.2.2'))
        self.assertEqual(version_neighbors('1'), ('1.0.1', '0.0.0'))
        self.assertEqual(version_neighbors('1.0.0'), ('1.0.1', '0.0.0'))
        self.assertEqual(version_neighbors('0.0.0'), ('0.0.1', None))
        self.assertEqual(version_neighbors('preview'), (None, None))
        self.assertEqual(version_neighbors('2147483647.0'), (None, None))

    def test_invalid_inputs_stop_before_build_or_fixture_changes(self):
        marker = self.root / 'untouched'
        marker.write_text('preserve')
        with mock.patch('run_offline.execute') as run, self.assertRaises(ValueError):
            main([str(self.root), str(self.base / 'missing.zip')])
        run.assert_not_called()
        self.assertEqual(marker.read_text(), 'preserve')

    def test_docker_commands_disable_network_and_pulling(self):
        script = 'printf "%s" "space; $quoted"'
        command = ChrootRunner(self.root, 'existing-image').command(script)
        self.assertEqual(command[command.index('--network') + 1], 'none')
        self.assertEqual(command[command.index('--pull') + 1], 'never')
        self.assertIn('existing-image', command)
        self.assertEqual(command[-1], script)
        build = build_command('existing-image', False)
        self.assertEqual(build[build.index('--network') + 1], 'none')
        self.assertEqual(build[build.index('--pull') + 1], 'never')

    def test_direct_mode_does_not_use_nested_docker(self):
        with mock.patch('offline_support.platform.system', return_value='Linux'), mock.patch('offline_support.os.geteuid', return_value=0):
            command = ChrootRunner(self.root, direct=True).command('echo test')
        self.assertEqual(command[:3], ['chroot', str(self.root), '/usr/bin/busybox.nosuid'])
        self.assertNotIn('docker', build_command('ignored', True))

    def test_direct_mode_requires_linux_root(self):
        with mock.patch('offline_support.platform.system', return_value='Darwin'), self.assertRaises(ValueError):
            ChrootRunner(self.root, direct=True)
        with mock.patch('offline_support.platform.system', return_value='Linux'), mock.patch('offline_support.os.geteuid', return_value=1000), self.assertRaises(ValueError):
            ChrootRunner(self.root, direct=True)

    def test_timeout_is_bounded(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            execute([sys.executable, '-c', 'import time; time.sleep(2)'], timeout=0.05)
        with self.assertRaises(ValueError):
            execute([sys.executable, '-c', 'pass'], timeout=0)


if __name__ == '__main__':
    unittest.main()

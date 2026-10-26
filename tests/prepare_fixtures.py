from __future__ import annotations

import hashlib
import json
import posixpath
import re
import shutil
import stat
import zipfile
from pathlib import Path, PurePosixPath

from offline_support import disposable_root

MAX_PACKAGE_SIZE = 512 * 1024 * 1024
MAX_METADATA_SIZE = 1024 * 1024


def stream_digest(stream) -> str:
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(65536), b''):
        result.update(block)
    return result.hexdigest()


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return stream_digest(stream)


def relative_path(value: object, *, single: bool = False) -> str:
    if not isinstance(value, str) or not value or any(ord(char) < 32 for char in value) or '\\' in value:
        raise ValueError('Invalid package path')
    parts = value.rstrip('/').split('/')
    if value.startswith('/') or any(part in ('', '.', '..') for part in parts) or (single and len(parts) != 1):
        raise ValueError('Unsafe package path')
    return value.rstrip('/')


def package_manifest(path: Path, *, legacy: bool = False) -> dict:
    path = Path(path).resolve()
    if not path.is_file():
        raise ValueError(f'Missing supplied package ZIP: {path}')
    if path.stat().st_size > MAX_PACKAGE_SIZE:
        raise ValueError('Package ZIP exceeds 512 MiB limit')
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if not entries or len(entries) >= 100000 or sum(entry.file_size for entry in entries) > MAX_PACKAGE_SIZE:
            raise ValueError('Package exceeds archive limits')
        members: dict[str, zipfile.ZipInfo] = {}
        roots: set[str] = set()
        for entry in entries:
            name = relative_path(entry.filename)
            if name in members or entry.flag_bits & 1:
                raise ValueError('Duplicate or encrypted ZIP entry')
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if kind not in (0, stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
                raise ValueError('Unsupported ZIP entry')
            members[name] = entry
            roots.add(name.split('/')[0])
        manifests = [name for name in members if name.count('/') == 1 and name.endswith('/mpc-plugin.json')]
        if len(roots) != 1 or len(manifests) != 1:
            raise ValueError('Supplied ZIP must contain one package folder and one package manifest')
        root = roots.pop()

        def member(name: str) -> zipfile.ZipInfo:
            entry = members.get(root + '/' + relative_path(name))
            if entry is None or entry.is_dir():
                raise ValueError(f'Missing package file: {name}')
            return entry

        def metadata(name: str) -> bytes:
            entry = member(name)
            if entry.file_size > MAX_METADATA_SIZE or stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError(f'Invalid metadata file: {name}')
            return archive.read(entry)

        value = json.loads(metadata('mpc-plugin.json'))
        if not isinstance(value, dict) or type(value.get('schema')) is not int or value['schema'] != 1 or value.get('arch') != 'armv7':
            raise ValueError('Expected schema 1 ARMv7 package manifest')
        for field in ('id', 'version', 'name', 'kind', 'uid', 'so', 'folder', 'skin', 'max_glibc'):
            text = value.get(field)
            if not isinstance(text, str) or not text or any(ord(char) < 32 for char in text):
                raise ValueError(f'Missing or invalid package {field}')
        if not re.fullmatch(r'[a-z0-9][a-z0-9_-]*', value['id']):
            raise ValueError('Invalid plugin ID')
        if value['kind'] not in ('instrument', 'effect') or not re.fullmatch(r'[0-9a-fA-F]{8}', value['uid']):
            raise ValueError('Expected instrument or effect with an eight-digit hexadecimal UID')
        if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', value['max_glibc']) or any(int(part) > 2147483647 for part in value['max_glibc'].split('.')):
            raise ValueError('Invalid package max_glibc')
        relative_path(value['folder'])
        relative_path(value['skin'], single=True)
        relative_path(value['so'], single=True)
        if PurePosixPath(value['folder']).name != value['skin']:
            raise ValueError('Package folder must end with its skin name')
        if legacy:
            if 'os_compat' in value:
                raise ValueError('Legacy package must omit os_compat')
        elif not isinstance(value.get('os_compat'), list) or '3.x' not in value['os_compat'] or not all(isinstance(item, str) and item and not any(ord(char) < 32 for char in item) for item in value['os_compat']):
            raise ValueError('Package must declare MPC OS 3 support')
        for script in ('install.sh', 'uninstall.sh'):
            if b'DEFER=' not in metadata(script):
                raise ValueError('Package scripts must support deferred MPC restart')
        metadata('plugin_list.awk')
        plugin = member(value['folder'] + '/' + value['so'])
        with archive.open(plugin) as stream:
            header = stream.read(20)
        if len(header) < 20 or header[:6] != b'\x7fELF\x01\x01' or int.from_bytes(header[18:20], 'little') != 40:
            raise ValueError('Plugin library must be a 32-bit little-endian ARM ELF')
        covered: dict[str, str] = {}
        for line in metadata('SHA256SUMS').decode('utf-8').splitlines():
            if len(line) <= 66 or not re.fullmatch(r'[0-9a-f]{64}', line[:64]) or line[64:66] not in ('  ', ' *'):
                raise ValueError('Invalid checksum file')
            name = relative_path(line[66:])
            if name in covered:
                raise ValueError('Duplicate package checksum')
            covered[name] = line[:64]
        for name, entry in members.items():
            if entry.is_dir():
                continue
            short = name[len(root) + 1:]
            if short == 'SHA256SUMS':
                continue
            target = entry
            visited = set()
            while stat.S_ISLNK(target.external_attr >> 16):
                if target.filename in visited or target.file_size > MAX_METADATA_SIZE:
                    raise ValueError('Invalid package symlink')
                visited.add(target.filename)
                link = archive.read(target).decode('utf-8')
                if not link or link.startswith('/') or '\\' in link:
                    raise ValueError('Unsafe package symlink')
                linked = posixpath.normpath(posixpath.join(posixpath.dirname(target.filename), link))
                if not linked.startswith(root + '/') or linked not in members:
                    raise ValueError('Package symlink escapes or has no target')
                target = members[linked]
            if target.is_dir():
                continue
            if short not in covered:
                raise ValueError('Unchecked package file: ' + short)
            with archive.open(target) as stream:
                actual = stream_digest(stream)
            if actual != covered[short]:
                raise ValueError('Package checksum mismatch: ' + short)
        for name in covered:
            member(name)
        return value


def version_neighbors(version: str) -> tuple[str | None, str | None]:
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*', version):
        return None, None
    parts = [int(part) for part in version.split('.')]
    if any(part >= 2147483647 for part in parts):
        return None, None
    while len(parts) < 3:
        parts.append(0)
    newer = parts.copy()
    newer[-1] += 1
    older = parts.copy()
    for index in range(len(older) - 1, -1, -1):
        if older[index]:
            older[index] -= 1
            older[index + 1:] = [0] * (len(older) - index - 1)
            return '.'.join(map(str, newer)), '.'.join(map(str, older))
    return '.'.join(map(str, newer)), None


def catalog_item(path: Path, manifest: dict, *, legacy: bool = False) -> dict:
    value = dict(manifest)
    value.update(author=manifest.get('author') or manifest.get('manufacturer') or '',
                 os_compat=['3.x'] if legacy else manifest['os_compat'], sha=digest(path),
                 size=str(path.stat().st_size), url='https://fixture/legacy.zip' if legacy else 'https://fixture/package.zip',
                 defer=True, origin='catalog')
    for field in ('author', 'summary', 'license'):
        if field in value and (not isinstance(value[field], str) or any(ord(char) < 32 for char in value[field])):
            raise ValueError(f'Invalid package {field}')
    return value


def prepare(root: Path, package: Path, legacy_package: Path | None = None, *, legacy_os3_confirmed: bool = False) -> None:
    root = disposable_root(root)
    package = Path(package).resolve()
    legacy_package = Path(legacy_package).resolve() if legacy_package else None
    if legacy_package is not None and not legacy_os3_confirmed:
        raise ValueError('Confirm legacy package OS 3 compatibility with --legacy-os3-confirmed')
    if legacy_os3_confirmed and legacy_package is None:
        raise ValueError('--legacy-os3-confirmed requires --legacy-package')
    for path in (package, legacy_package):
        if path is not None and path.is_relative_to(root):
            raise ValueError('Input ZIPs must be outside the disposable fixture root')
    primary = catalog_item(package, package_manifest(package))
    legacy = catalog_item(legacy_package, package_manifest(legacy_package, legacy=True), legacy=True) if legacy_package else None
    _prepare(root, package, legacy_package, primary, legacy)


def _prepare(root: Path, package: Path, legacy_package: Path | None, primary: dict, legacy: dict | None) -> None:
    for name in ('manager-usb', 'manager-session', 'manager-fixtures', 'manager-test-bin'):
        target = root / 'tmp' / name
        if target.is_symlink():
            target.unlink()
        elif target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
    usb = root / 'tmp/manager-usb'
    (usb / 'local-plugins').mkdir()
    shutil.copyfile(package, usb / 'local-plugins/package.zip')
    (usb / 'local-plugins/._ignored.zip').write_bytes(b'Mac metadata')
    (usb / '.mpc-manager-id').write_text('mpc-usb-manager-v1\n')
    fixtures = root / 'tmp/manager-fixtures'
    shutil.copyfile(package, fixtures / 'package.zip')
    if legacy_package is not None:
        shutil.copyfile(legacy_package, fixtures / 'legacy.zip')
    with package.open('rb') as stream:
        (fixtures / 'partial.zip').write_bytes(stream.read(4096))
    newer, older = version_neighbors(primary['version'])
    (fixtures / 'fixture.json').write_text(json.dumps({'primary': primary, 'legacy': legacy, 'newer_version': newer, 'older_version': older}) + '\n')
    fields = ['plugin', primary['id'], primary['version'], '1', primary['kind'], primary['name'], primary['skin'], primary['uid'], '1', primary['size'], primary['sha'], primary['url'], '-', '1', ','.join(primary['os_compat']), primary['max_glibc']]
    (fixtures / 'catalog.tsv').write_text('#mpc-catalog-tsv 1\n' + '\t'.join(fields) + '\n')
    details = {'id': primary['id'], 'author': primary['author'], 'summary': primary.get('summary', primary.get('about', '')), 'license': primary.get('license', ''), 'versions': [{'version': primary['version'], 'sha256': primary['sha']}]}
    (fixtures / 'catalog.json').write_text(json.dumps({'schema': 1, 'plugins': [details]}) + '\n')
    with zipfile.ZipFile(package) as archive:
        folder = archive.namelist()[0].split('/')[0]
    for name, entry in [('traversal.zip', folder + '/../../escape'), ('absolute.zip', '/tmp/escape'), ('multi-root.zip', folder + '-other/file')]:
        shutil.copyfile(package, fixtures / name)
        with zipfile.ZipFile(fixtures / name, 'a') as archive:
            archive.writestr(entry, 'attack')
    shutil.copyfile(package, fixtures / 'escape-link.zip')
    with zipfile.ZipFile(fixtures / 'escape-link.zip', 'a') as archive:
        entry = zipfile.ZipInfo(folder + '/manager-test-escape-link')
        entry.create_system = 3
        entry.external_attr = 0o120777 << 16
        archive.writestr(entry, '../../escape')
    shutil.copyfile(package, fixtures / 'oversize.zip')
    with zipfile.ZipFile(fixtures / 'oversize.zip', 'a', compression=zipfile.ZIP_DEFLATED) as archive:
        with archive.open(folder + '/manager-test-large', 'w') as stream:
            for _ in range(513):
                stream.write(bytes(1024 * 1024))
    shutil.copyfile(fixtures / 'traversal.zip', usb / 'local-plugins/bad.zip')
    (root / 'tmp/manager-test-bin/wget').write_text('''#!/bin/sh
set -eu
printf 'request\\n' >> /tmp/manager-fixtures/requests
case "$5" in
  https://sd88me.github.io/mpc-vst-plugins/catalog.tsv) cp /tmp/manager-fixtures/catalog.tsv "$4" ;;
  https://sd88me.github.io/mpc-vst-plugins/catalog.json) cp /tmp/manager-fixtures/catalog.json "$4" ;;
  https://fixture/legacy.zip) cp /tmp/manager-fixtures/legacy.zip "$4" ;;
  https://fixture/package.zip) cp /tmp/manager-fixtures/partial.zip "$4"; sleep 0.1; cp /tmp/manager-fixtures/package.zip "$4"; sleep 0.1 ;;
  *) exit 1 ;;
esac
''')
    (root / 'tmp/manager-test-bin/wget').chmod(0o755)
    (root / 'tmp/manager-settings.xml').write_text('''<PROPERTIES>
<VALUE name="SynthContentLocations"><SynthContentLocations><Location>/sdcard/Expansions</Location></SynthContentLocations></VALUE>
<VALUE name="pluginList-arm"><KNOWNPLUGINS/></VALUE>
</PROPERTIES>
''')
    (root / 'sdcard/Expansions').mkdir(parents=True, exist_ok=True)
    owned = {primary['skin'], '.' + primary['skin'] + '.new', '.' + primary['skin'] + '.old', '.mpc-usb-manager'}
    if legacy:
        owned.update((legacy['skin'], '.' + legacy['skin'] + '.new', '.' + legacy['skin'] + '.old'))
    for path in (root / 'sdcard/Expansions').iterdir():
        if path.name in owned:
            if path.is_symlink() or path.is_file():
                path.unlink()
            else:
                shutil.rmtree(path)
    (root / 'proc/mounts').write_text('/dev/root / ext4 ro 0 0\n/dev/mmcblk1p1 /sdcard ext4 rw,relatime 0 0\n')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=disposable_root)
    parser.add_argument('package_zip', type=Path)
    parser.add_argument('--legacy-package', type=Path)
    parser.add_argument('--legacy-os3-confirmed', action='store_true')
    args = parser.parse_args()
    prepare(args.root, args.package_zip, args.legacy_package, legacy_os3_confirmed=args.legacy_os3_confirmed)

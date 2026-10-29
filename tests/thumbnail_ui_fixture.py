import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    binary, font, output = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    output.mkdir(exist_ok=False)
    digest = 'a' * 64
    rows, plugins = [], []
    for index in range(6):
        identity = 'fixture-' + chr(97 + index)
        name = 'Generic ' + chr(65 + index)
        version = '1.0.3' if index == 0 else '1.0.0'
        row = ['plugin', identity, version, '1', 'instrument', name, identity,
               'Fi' + str(index).zfill(2), '1', '1000', digest,
               'https://package.invalid/module.zip', '-', '1', '3.x', '2.17',
               '1000', digest, 'https://package.invalid/module64.zip']
        rows.append('\t'.join(row))
        plugins.append({'id': identity, 'author': 'Fixture author',
                        'screenshot': 'https://artwork.invalid/valid.png' if index < 5 else '',
                        'versions': [{'version': version, 'sha256': digest,
                                      'assets': {'aarch64': {'sha256': digest}}}]})
    (output / 'catalog.tsv').write_text('#mpc-catalog-tsv 1\n' + '\n'.join(rows) + '\n')
    (output / 'catalog.json').write_text(json.dumps({'schema': 1, 'plugins': plugins}))
    tools = output / 'bin'
    tools.mkdir()
    wget = tools / 'wget'
    wget.write_text('#!/bin/sh\ncase "$5" in */catalog.tsv) cp "$THUMBNAIL_UI_ROOT/catalog.tsv" "$4";; */catalog.json) cp "$THUMBNAIL_UI_ROOT/catalog.json" "$4";; *) exit 90;; esac\n')
    wget.chmod(0o755)
    capture_spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('sd_ui_test.py'))
    capture = importlib.util.module_from_spec(capture_spec)
    capture_spec.loader.exec_module(capture)
    results = []
    for dark in (True, False):
        for view in ('catalog', 'installed', 'history'):
            root = output / (view + ('-dark' if dark else '-light'))
            source, session, synths = (root / name for name in ('source', 'session', 'synths'))
            for directory in (source, session, synths):
                directory.mkdir(parents=True)
            (source / '.mpc-manager-id').write_text('mpc-usb-manager-v1\n')
            (source / 'manager-preferences.json').write_text(json.dumps({'schema': 1, 'dark_theme': dark, 'hide_welcome': True}))
            history = {}
            for index in range(6):
                identity = 'fixture-' + chr(97 + index)
                history[identity] = {'id': identity, 'skin': identity, 'name': 'Generic ' + chr(65 + index),
                                     'version': '1.0.1' if index == 0 else '1.0.0', 'kind': 'instrument'}
                if index not in (0, 1, 4):
                    continue
                directory = synths / identity
                directory.mkdir()
                (directory / 'module.so').write_text('File presence marker; not executable')
                (directory / 'mpc-plugin.json').write_text(json.dumps({
                    'id': identity, 'skin': identity, 'name': 'Generic ' + chr(65 + index),
                    'version': '1.0.2' if index == 0 else '1.0.0', 'so': 'module.so', 'kind': 'instrument'}))
            (source / 'plugin-history.json').write_text(json.dumps({'schema': 1, 'plugins': history}))
            settings = root / 'MPC.settings'
            settings.write_text('<PROPERTIES/>')
            env = dict(os.environ, PATH=str(tools) + ':' + os.environ['PATH'],
                       THUMBNAIL_UI_ROOT=str(output), SDL_VIDEODRIVER='dummy',
                       MPC_MANAGER_UI_TEST='thumbnail-capture', MPC_MANAGER_RENDER_VIEW=view)
            result = subprocess.run([str(binary), str(source), str(session), str(synths), str(settings), str(font)],
                                    env=env, capture_output=True, text=True, timeout=15)
            (root / 'manager.log').write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError('Native thumbnail capture failed: ' + str(root / 'manager.log'))
            bmp = session / 'screen.bmp'
            data = bmp.read_bytes()
            offset = struct.unpack_from('<I', data, 10)[0]
            width, height = struct.unpack_from('<ii', data, 18)
            if (width, abs(height)) != (1280, 800):
                raise RuntimeError('Unexpected native canvas size')
            def pixel(x, y):
                row = height - 1 - y if height > 0 else y
                return struct.unpack_from('<I', data, offset + (row * width + x) * 4)[0]
            background, line = (0xff242321, 0xff55514b) if dark else (0xffe9e7e2, 0xffcdcac2)
            if pixel(28, 142) != background or pixel(28, 143) != line:
                raise RuntimeError('Native grid top clearance differs from approved layout')
            if view != 'installed' and (pixel(28, 584) != line or pixel(28, 585) != background or pixel(28, 604) != background):
                raise RuntimeError('Native grid bottom clearance differs from approved layout')
            if (pixel(600, 170) == (0xffd94a3f if dark else 0xffd63a30)) != (view == 'catalog'):
                raise RuntimeError('Native update LED semantics differ by view')
            image = capture.compact_capture(bmp)
            results.append({'theme': 'dark' if dark else 'light', 'view': view,
                            'image': str(image.relative_to(output)), 'returncode': result.returncode})
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: six native SDL captures; card clearances, themes and per-view LED placement')


if __name__ == '__main__':
    main()

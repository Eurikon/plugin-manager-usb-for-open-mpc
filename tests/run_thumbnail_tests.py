import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('src/thumbnail.hpp', 'src/thumbnail.cpp', 'tests/thumbnail_test.cpp',
           'tests/thumbnail_network_test.cpp', 'tests/thumbnail_https_fixture.py')


def main():
    parser = argparse.ArgumentParser(description='Offline native thumbnail and loopback HTTPS checks')
    parser.add_argument('--arch', choices=('armv7', 'aarch64'), default='aarch64')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        parser.error('Use a fresh output directory inside build/')
    output.mkdir(parents=True, exist_ok=False)
    data = {name: (ROOT / name).read_bytes() for name in SOURCES}
    source = output / 'source'
    for name, content in data.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    image = 'mpc-usb-plugin-installer-build-tools' + (':aarch64' if args.arch == 'aarch64' else '')
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
    compile_prefix = ' '.join(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
                              '-Isrc', '$(pkg-config --cflags libpng)', 'src/thumbnail.cpp'])
    links = '-Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic -ldl -pthread'
    script = '\n'.join([
        compile_prefix + ' tests/thumbnail_test.cpp ' + links + ' -o /results/thumbnail-test',
        compile_prefix + ' tests/thumbnail_network_test.cpp ' + links + ' -o /results/thumbnail-network-test',
        'ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 /results/thumbnail-test',
        'ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 python3 tests/thumbnail_https_fixture.py /results/thumbnail-network-test'])
    container = 'pmu-thumbnail-' + uuid.uuid4().hex[:12]
    command = ['docker', 'run', '--rm', '--pull=never', '--name', container,
               '--platform=' + ('linux/arm64' if args.arch == 'aarch64' else 'linux/arm/v7'),
               '--network=none', '--memory=512m', '--pids-limit=128',
               '--add-host=artwork.invalid:127.0.0.1', '--add-host=wrong-name.invalid:127.0.0.1',
               '--mount', f'type=bind,src={source},dst=/source,readonly',
               '--mount', f'type=bind,src={output},dst=/results', '-w', '/source', image,
               'sh', '-ec', script]
    report = {'date': datetime.now().strftime('%d-%m-%Y'), 'arch': args.arch,
              'sanitizers': args.sanitize, 'scope': 'Offline thumbnail loader; ephemeral loopback TLS only',
              'files': {name: hashlib.sha256(content).hexdigest() for name, content in data.items()},
              'command': command}
    try:
        with (output / 'tests.log').open('x') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=240)
        text = (output / 'tests.log').read_text()
        report['returncode'] = result.returncode
        report['passed'] = result.returncode == 0 and not re.search(
            r'warning:|runtime error:|AddressSanitizer:|^FAIL:', text, re.M)
        report['checks'] = sum(map(int, re.findall(r'PASS: (\d+)', text)))
        if not report['passed'] or report['checks'] < 300:
            raise RuntimeError('Thumbnail verification failed; inspect ' + str(output / 'tests.log'))
        print(f"PASS: {args.arch}, {report['checks']} checks" + (', fatal ASan/UBSan' if args.sanitize else ''))
    finally:
        subprocess.run(['docker', 'rm', '-f', container], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()

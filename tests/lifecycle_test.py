import shutil
from pathlib import Path
from offline_support import ChrootRunner, lifecycle_arguments

args = lifecycle_arguments()
root = args.root
runner = ChrootRunner(root, args.image, args.direct, args.timeout)
base = root/'tmp/lifecycle-test'
if base.exists(): shutil.rmtree(base)
base.mkdir()
usb=base/'usb';usb.mkdir();(usb/'.mpc-manager-id').write_text('test')
bin_dir=base/'bin';bin_dir.mkdir()
(bin_dir/'systemctl').write_text('''#!/bin/sh
printf '%s\\n' "$1" >> /tmp/lifecycle-test/service.log
if [ "$1" = start ] && grep -q 'Manager exited unexpectedly' /tmp/lifecycle-test/usb/manager-result.txt 2>/dev/null; then echo failure-recorded >> /tmp/lifecycle-test/service.log; fi
exit 0
''')
(bin_dir/'pidof').write_text('#!/bin/sh\nexit 1\n')
(bin_dir/'wget').write_text('#!/bin/sh\nexit 1\n')
for path in bin_dir.iterdir():path.chmod(0o755)
settings=root/'media/az01-internal/Settings/MPC/MPC.settings'
settings.parent.mkdir(parents=True,exist_ok=True)
settings.write_text('<PROPERTIES/>\n')
(root/'sdcard/Expansions').mkdir(parents=True,exist_ok=True)
launch=base/'launch.sh';shutil.copyfile(Path(__file__).parents[1]/'launch.sh',launch)
for name,behavior,expected in [('normal','exit 0',0),('display-failure','exit 1',1),('manager-crash','kill -KILL $$',137),('real-display-failure',None,1),('real-normal','export SDL_VIDEODRIVER=dummy MPC_MANAGER_UI_TEST=smoke; exec /tmp/manager-ui "$@"',0),('real-usb-removal','export SDL_VIDEODRIVER=dummy; (sleep 1; rm "$1/.mpc-manager-id") & exec /tmp/manager-ui "$@"',0)]:
    (usb/'.mpc-manager-id').write_text('test')
    work=base/name;work.mkdir();(work/'session.log').write_text('')
    if behavior is None: shutil.copyfile(Path(__file__).parents[1]/'build/manager',work/'manager')
    else: (work/'manager').write_text('#!/bin/sh\nmkdir -p "$2"\nprintf catalog > "$2/catalog.tsv"\n'+behavior+'\n')
    shutil.copyfile(Path(__file__).parents[1]/'build/font.ttf',work/'font.ttf')
    (work/'manager').chmod(0o755)
    result=runner.run(f'PATH=/tmp/lifecycle-test/bin:$PATH sh /tmp/lifecycle-test/launch.sh /tmp/lifecycle-test/usb /tmp/lifecycle-test/{name}')
    assert result.returncode==expected,(name,result.stdout,result.stderr,result.returncode)
    assert not work.exists(),name
    if name not in ['real-usb-removal']: assert (usb/'manager-result.txt').exists(),name
    if expected: assert 'Manager exited unexpectedly' in (usb/'manager-result.txt').read_text(),name
    print('PASS:',name,'restarts MPC and deletes RAM session')
assert (base/'service.log').read_text().splitlines().count('stop')==6
assert (base/'service.log').read_text().splitlines().count('start')==6
assert (base/'service.log').read_text().splitlines().count('failure-recorded')==3
print('PASS: unexpected exit and killed manager recorded before MPC restart; one stop/start per session')

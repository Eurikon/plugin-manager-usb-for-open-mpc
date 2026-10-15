import shutil
from pathlib import Path
from offline_support import ChrootRunner, lifecycle_arguments

args=lifecycle_arguments()
root=args.root
runner=ChrootRunner(root,args.image,args.direct,args.timeout)
source=Path(__file__).parents[1]
test=root/'tmp/shortcut-test'
if test.exists():shutil.rmtree(test)
test.mkdir();bin_dir=test/'bin';bin_dir.mkdir()
for name in ['shortcut','shortcut-hook.so','shortcut-state-test','shortcut-fake.so','shortcut-addin-first.so','shortcut-addin-second.so','shortcut-hook-test','shortcut-integration']:
    shutil.copyfile(source/'build'/name,test/name);(test/name).chmod(0o755)
shutil.copyfile(source/'launch.sh',test/'launch.sh')
shutil.copyfile(source/'setup.sh',test/'setup.sh')
(test/'usb/autorun').mkdir(parents=True)
shutil.copyfile(source/'open.sh',test/'usb/autorun/open.sh')
shutil.copyfile(source/'build/font.ttf',test/'font.ttf')
(test/'manager').write_text('#!/bin/sh\necho opened >> /tmp/shortcut-test/launches\nsleep 0.5\n')
(test/'manager').chmod(0o755)
(bin_dir/'pidof').write_text('#!/bin/sh\nexit 1\n')
(bin_dir/'systemctl').write_text('''#!/bin/sh
echo "$1" >> /tmp/shortcut-test/services
case "$1" in
show)
    case "$*" in
    'show acvs -p MainPID --value')
        [ ! -e /tmp/shortcut-test/fail-show ] || exit 92
        if [ -e /tmp/shortcut-test/pid-race ] && [ -e /tmp/shortcut-test/pid-queried ]; then
            echo 4243
        else
            touch /tmp/shortcut-test/pid-queried
            cat /tmp/shortcut-test/main-pid
        fi ;;
    'show acvs -p UnsetEnvironment --value')
        if [ -e /tmp/shortcut-test/unset-variable ]; then cat /tmp/shortcut-test/unset-variable; else echo UNRELATED; fi ;;
    'show acvs -p EnvironmentFiles --value')
        printf '/tmp/shortcut-test/existing.env (ignore_errors=no) /tmp/mpc-usb-session.test/shortcut.env (ignore_errors=no)'
        if [ -e /tmp/shortcut-test/late-envfile ]; then printf ' /tmp/shortcut-test/existing.env (ignore_errors=no)'; fi
        printf '\\n' ;;
    *) exit 91 ;;
    esac ;;
daemon-reload) [ ! -e /tmp/shortcut-test/fail-reload ] || exit 1 ;;
esac
exit 0
''')
(bin_dir/'amidi').write_text("#!/bin/sh\nprintf 'Dir Device Name\\nIO hw:1,0,1 MPC Private\\n'\n")
(bin_dir/'systemd-run').write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> /tmp/shortcut-test/dispatch\nexit 0\n')
for path in bin_dir.iterdir():path.chmod(0o755)
settings=root/'media/az01-internal/Settings/MPC/MPC.settings';settings.parent.mkdir(parents=True,exist_ok=True);settings.write_text('<PROPERTIES/>\n')
(root/'sdcard/Expansions').mkdir(parents=True,exist_ok=True)
def run(command):
    return runner.run(command)
hook=run('LD_PRELOAD=/tmp/shortcut-test/shortcut-hook.so MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/shortcut-test/hook.sock /tmp/shortcut-test/shortcut-hook-test')
print(hook.stdout,end='');assert hook.returncode==0,hook.stderr
chained=run("LD_PRELOAD='/tmp/shortcut-test/shortcut-addin-first.so /tmp/shortcut-test/shortcut-addin-second.so /tmp/shortcut-test/shortcut-hook.so' MPC_PANEL_PORT=hw:1,0,1 MPC_SHORTCUT_SOCKET=/tmp/shortcut-test/hook.sock /tmp/shortcut-test/shortcut-hook-test --chained")
print(chained.stdout,end='');assert chained.returncode==0,chained.stderr
result=run('PATH=/tmp/shortcut-test/bin:$PATH /tmp/shortcut-test/shortcut-state-test && PATH=/tmp/shortcut-test/bin:$PATH /tmp/shortcut-test/shortcut-integration')
print(result.stdout,end='');assert result.returncode==0,result.stderr
calls=(test/'services').read_text().splitlines();assert calls.count('stop')==6 and calls.count('start')==6,calls
assert not list((root/'tmp').glob('mpc-usb-manager.*')),'Manager RAM session left behind'
print('PASS: one MPC stop/start per opening; all GUI RAM sessions deleted')
for case in ['preload','envfile','normal','arm-only','unknown-option']:
    work=root/f'tmp/mpc-usb-session.setup-{case}';work.mkdir()
    option=' --arm-only' if case=='arm-only' else ' --unknown' if case=='unknown-option' else ''
    result=run(f'PATH=/tmp/shortcut-test/bin:$PATH SHORTCUT_CASE={case} sh /tmp/shortcut-test/setup.sh /tmp/shortcut-test/usb /tmp/mpc-usb-session.setup-{case}{option}')
    assert result.returncode==(1 if case=='unknown-option' else 0),(case,result.stdout,result.stderr)
    if case!='unknown-option':shutil.rmtree(work)
    else:assert not work.exists()
    print('PASS: bootstrap',case,'configuration handling')
dispatch=(test/'dispatch').read_text().splitlines();assert dispatch.count('--arm-only')==1,dispatch
invalid=run('PATH=/tmp/shortcut-test/bin:$PATH /tmp/shortcut-test/shortcut /tmp/shortcut-test/usb /tmp/mpc-usb-session.invalid acvs hw:1,0,1 --unknown')
assert invalid.returncode==1 and not (root/'tmp/mpc-usb-session.invalid').exists(),invalid.stderr
print('PASS: unknown supervisor option rejected before session setup')

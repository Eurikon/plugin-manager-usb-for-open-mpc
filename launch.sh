#!/bin/sh
set -eu
usb=$1
work=$2
service=acvs
child=''
restore() {
    result=$?
    trap - EXIT INT TERM HUP
    [ -z "$child" ] || { kill "$child" 2>/dev/null || true; wait "$child" 2>/dev/null || true; }
    if [ -f "$usb/.mpc-manager-id" ]; then cp "$work/session.log" "$usb/manager-result.txt" 2>/dev/null || true; fi
    if ! rm -rf "$work"; then
        if [ -f "$usb/.mpc-manager-id" ]; then echo 'RAM cleanup failed' >> "$usb/manager-result.txt"; fi
    fi
    systemctl start "$service" || true
    exit "$result"
}
trap restore EXIT
trap 'exit 1' INT TERM HUP
exec >> "$work/session.log" 2>&1
printf 'USB manager started: '; date '+%d-%m-%Y %H:%M:%S'
[ "$(id -u)" = 0 ] || { echo 'Root required'; exit 1; }
case "$(uname -m)" in armv7*|aarch64) ;; *) echo 'Unsupported architecture'; exit 1 ;; esac
systemctl cat acvs >/dev/null 2>&1 || service=inmusic-mpc
case "$(uname -m)" in
    aarch64) settings=/data/Settings/MPC/MPC.settings; target=auto ;;
    *) settings=$(for file in /media/az01-internal/Settings/*/MPC.settings; do
           [ ! -f "$file" ] || { printf '%s\n' "$file"; break; }
       done); target=/sdcard/Expansions ;;
esac
[ -f "$settings" ] && { [ "$target" = auto ] || [ -d "$target" ]; } || { echo 'Settings or plugin content folder missing'; exit 1; }
systemctl stop "$service"
i=0
while pidof MPC >/dev/null && [ "$i" -lt 30 ]; do sleep 1; i=$((i+1)); done
if pidof MPC >/dev/null; then echo 'MPC did not stop'; exit 1; fi
export SDL_VIDEODRIVER=kmsdrm
export SDL_KMSDRM_REQUIRE_DRM_MASTER=1
export SDL_TOUCH_MOUSE_EVENTS=0
"$work/manager" "$usb" "$work/cache" "$target" "$settings" "$work/font.ttf" &
child=$!
if wait "$child"; then result=0; else result=$?; fi
child=''
if [ "$result" -ne 0 ]; then
    echo "Manager exited unexpectedly (status $result); restarting MPC immediately"
    exit "$result"
fi
echo 'Manager closed; RAM catalog and downloads discarded'

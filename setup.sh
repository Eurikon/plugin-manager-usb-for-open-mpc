#!/bin/sh
set -eu
usb=$1
work=$2
trap 'rm -rf "$work"' EXIT
case "$#:${3-}" in 2:) ;; 3:--arm-only) ;; *) echo 'Expected USB RAM_SESSION [--arm-only]'; exit 1 ;; esac
service=acvs
[ "$(id -u)" = 0 ] || exit 1
systemctl cat acvs >/dev/null 2>&1 || service=inmusic-mpc
ports=$(amidi -l) || { echo 'MIDI port enumeration failed'; exit 1; }
port=$(printf '%s\n' "$ports" | awk '
    ($1 == "I" || $1 == "IO") && $2 ~ /^hw:[0-9]+,[0-9]+,[0-9]+$/ && NF >= 3 && $NF ~ /Private$/ {
        port = $2
        count++
    }
    END { if (count == 1) print port; else exit 1 }
') || { echo 'Private MIDI input port not found or ambiguous'; exit 1; }
if [ "$#" -eq 3 ]; then arm=--arm-only; else arm=''; fi
set -- "$work/shortcut" "$usb" "$work" "$service" "$port"
[ -z "$arm" ] || set -- "$@" "$arm"
case "${MPC_MANAGER_SD_HELPER-}" in
    '') ;;
    '/sdcard/MPC Documents/Devices/Plugin Manager helper')
        set -- "--setenv=MPC_MANAGER_SD_HELPER=$MPC_MANAGER_SD_HELPER" "$@" ;;
    *) echo 'Unsupported SD installation helper'; exit 1 ;;
esac
systemd-run --unit=mpc-usb-manager --collect "$@"
trap - EXIT

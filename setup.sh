#!/bin/sh
set -eu
usb=$1
work=$2
trap 'rm -rf "$work"' EXIT
case "$#:${3-}" in 2:) ;; 3:--arm-only) ;; *) echo 'Expected USB RAM_SESSION [--arm-only]'; exit 1 ;; esac
service=acvs
[ "$(id -u)" = 0 ] || exit 1
systemctl cat acvs >/dev/null 2>&1 || service=inmusic-mpc
environment=$(systemctl show "$service" -p Environment -p EnvironmentFiles)
files=$(printf '%s\n' "$environment" | sed -n 's/^EnvironmentFiles=//p')
[ -z "$files" ] || { echo 'Existing environment-file setup is unsupported'; exit 1; }
case "$environment" in *LD_PRELOAD=*) echo 'Existing preload setup is unsupported'; exit 1 ;; esac
ports=$(amidi -l) || { echo 'MIDI port enumeration failed'; exit 1; }
port=$(printf '%s\n' "$ports" | awk '
    ($1 == "I" || $1 == "IO") && $2 ~ /^hw:[0-9]+,[0-9]+,[0-9]+$/ && NF >= 3 && $NF ~ /Private$/ {
        port = $2
        count++
    }
    END { if (count == 1) print port; else exit 1 }
') || { echo 'Private MIDI input port not found or ambiguous'; exit 1; }
if [ "$#" -eq 3 ]; then
    systemd-run --unit=mpc-usb-manager --collect "$work/shortcut" "$usb" "$work" "$service" "$port" --arm-only
else
    systemd-run --unit=mpc-usb-manager --collect "$work/shortcut" "$usb" "$work" "$service" "$port"
fi
trap - EXIT

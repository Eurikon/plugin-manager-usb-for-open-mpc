#!/bin/sh
set -eu
here=$(CDPATH= cd "$(dirname "$0")" && pwd)
usb=$(dirname "$here")
state=/run/mpc-usb-manager-session
if [ -f "$state" ]; then
    read -r active < "$state"
    if [ -x "$active/shortcut" ] && "$active/shortcut" --request "$active/shortcut.sock"; then exit 0; fi
fi
work=$(mktemp -d /tmp/mpc-usb-session.XXXXXX)
trap 'rm -rf "$work"' EXIT
cp -R "$here/runtime/." "$work/"
chmod 755 "$work/manager" "$work/shortcut"
trap - EXIT
exec sh "$work/setup.sh" "$usb" "$work"

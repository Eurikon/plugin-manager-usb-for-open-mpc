#!/bin/sh
set -eu
here=$(CDPATH= cd "$(dirname "$0")" && pwd)
usb=$(dirname "$here")
case "$(uname -m)" in
    armv7*) arch=armv7 ;;
    aarch64) arch=aarch64 ;;
    *) echo 'Unsupported MPC architecture'; exit 1 ;;
esac
if [ "$arch" = aarch64 ] && [ ! -d "$here/runtime/aarch64" ]; then
    echo 'This bundle has no Gen 2 runtime'; exit 1
fi
state=/run/mpc-usb-manager-session
if [ -f "$state" ]; then
    read -r active < "$state"
    if [ -x "$active/shortcut" ] && "$active/shortcut" --request "$active/shortcut.sock"; then exit 0; fi
fi
work=$(mktemp -d /tmp/mpc-usb-session.XXXXXX)
trap 'rm -rf "$work"' EXIT
cp -R "$here/runtime/." "$work/"
if [ -d "$work/$arch" ]; then
    for name in manager shortcut shortcut-hook.so; do cp "$work/$arch/$name" "$work/$name"; done
fi
rm -rf "$work/armv7" "$work/aarch64"
chmod 755 "$work/manager" "$work/shortcut"
trap - EXIT
exec sh "$work/setup.sh" "$usb" "$work"

#!/bin/sh
set -eu
umask 077
PATH='/usr/sbin:/usr/bin:/sbin:/bin'
export PATH
SOURCE='/sdcard/MPC Documents/Devices/Plugin Manager helper'
MPC='/usr/bin/MPC'
EXPECTED='753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494'
OWNER_UID=0
MEDIA='/media'
MOUNTS='/proc/mounts'
UUIDS='/dev/disk/by-uuid'
INTERNAL='/sdcard'
SD_SUFFIX='/Plugin Manager USB'
RUN='/run'
PROC='/proc'
RAM_TEMPLATE='/tmp/mpc-usb-session.XXXXXX'
USB_WAIT=20
READY_WAIT=15
POLL_DELAY=1
FILES='manager font.ttf launch.sh setup.sh shortcut shortcut-hook.so'
PROJECT_BYTES=48697
PROJECT_SHA256='b8182372a271cc8b3d5c9e50af8c84012fc142d6894b516abc4f7bdd9f0fa830'
PAYLOAD=''
ARCH=armv7
if [ "$#" = 7 ]; then
    [ "$5" = --pmu-helper-v1 ] || exit 10
    SOURCE=$6 PAYLOAD=$7
    case "$SOURCE" in
        '/sdcard/MPC Documents/Devices/Plugin Manager helper'|'/nvme/MPC Documents/Devices/Plugin Manager helper'|'/storage/MPC Documents/Devices/Plugin Manager helper'|'/data/UserData/MPC Documents/Devices/Plugin Manager helper') ;;
        *) exit 11 ;;
    esac
    case "$PAYLOAD" in "$SOURCE"/.pmu-versions/*) ;; *) exit 11 ;; esac
    generation=${PAYLOAD#"$SOURCE"/.pmu-versions/}
    [ "${#generation}" = 64 ] || exit 11
    case "$generation" in *[!0-9a-f]*) exit 11 ;; esac
    set -- "$1" "$2" "$3" "$4"
fi

MANIFEST=runtime.sha256
NOTE_WRITER=bootstrap-note
SOURCE_EXFAT=0
case "$(uname -m)" in
    aarch64)
        ARCH=aarch64
        [ -n "$PAYLOAD" ] || SOURCE=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
        case "$SOURCE" in
            '/nvme/MPC Documents/Devices/Plugin Manager helper'|'/sdcard/MPC Documents/Devices/Plugin Manager helper'|'/storage/MPC Documents/Devices/Plugin Manager helper'|'/data/UserData/MPC Documents/Devices/Plugin Manager helper') ;;
            *) printf '%s\n' 'unsupported helper location' >&2; exit 11 ;;
        esac
        EXPECTED='b80687ed4f6b5233486a9e20ff043b35e9863bc6460bd573a249421133d77b63'
        INTERNAL=${SOURCE%'/MPC Documents/Devices/Plugin Manager helper'}
        MANIFEST=runtime-aarch64.sha256
        NOTE_WRITER=bootstrap-note-aarch64
        FILES='aarch64/manager font.ttf launch.sh setup.sh aarch64/shortcut aarch64/shortcut-hook.so'
        ;;
esac
if [ -n "$PAYLOAD" ] && [ "$ARCH" = armv7 ] && [ "$SOURCE" != '/sdcard/MPC Documents/Devices/Plugin Manager helper' ]; then exit 11; fi

failure_note() {
    [ -n "${confirmed_usb-}" ] && [ -d "$SOURCE" ] && [ ! -L "$SOURCE" ] || return 0
    regular_source "$SOURCE/$NOTE_WRITER" || return 0
    chmod 700 "$SOURCE/$NOTE_WRITER" || return 0
    "$SOURCE/$NOTE_WRITER" "$confirmed_usb" "${confirmed_identity-}" "$2" "$1"
}
fail() { printf '%s\n' "$1" >&2; failure_note "$1" "$2" 2>/dev/null || true; exit "$2"; }
regular_source() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(stat -c %u "$1")" = "$OWNER_UID" ] || return 1
    mode=$(stat -c %a "$1")
    case "$mode" in ''|*[!0-7]*) return 1 ;; esac
    [ "${#mode}" -le 3 ] || return 1
    [ "$((0$mode & 0422))" = 256 ] && return 0
    case "$SOURCE_EXFAT:$1" in
        1:"$SOURCE"/*) [ "$(stat -c %d "$1")" = "$(stat -c %d "$SOURCE")" ] ;;
        *) return 1 ;;
    esac
}
remove_import_project() {
    project="$SOURCE/PluginManager.xpj"
    regular_source "$project" || return 0
    [ "$(stat -c %h "$project")" = 1 ] \
        && [ "$(stat -c %s "$project")" = "$PROJECT_BYTES" ] || return 0
    identity=$(stat -c '%d:%i:%s:%Y:%Z:%h' "$project") || return 0
    digest=$(dd if="$project" bs="$((PROJECT_BYTES + 1))" count=1 2>/dev/null | sha256sum) || return 0
    [ "${digest%% *}" = "$PROJECT_SHA256" ] || return 0
    regular_source "$project" \
        && [ "$(stat -c '%d:%i:%s:%Y:%Z:%h' "$project")" = "$identity" ] || return 0
    rm -f -- "$project" || true
}
idle_session() {
    for path in "$RUN/mpc-usb-manager-session" \
        "$RUN/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf" \
        "$RUN/systemd/system/inmusic-mpc.service.d/99-mpc-usb-shortcut.conf"; do
        [ ! -e "$path" ] && [ ! -L "$path" ] || fail 'existing manager session preserved' 14
    done
    status=0
    snapshot=$(systemctl show mpc-usb-manager.service -pLoadState -pActiveState -pMainPID) || status=$?
    [ "$status" = 0 ] || [ "$status" = 4 ] || fail 'manager service inspection failed' 14
    load='' active='' pid=''
    while IFS='=' read -r key value; do
        case "$key" in
            LoadState) load=$value ;; ActiveState) active=$value ;; MainPID) pid=$value ;;
            *) fail 'invalid manager service state' 14 ;;
        esac
    done <<STATE
$snapshot
STATE
    case "$load:$active:$pid" in
        not-found:inactive:0) ;;
        *) fail 'existing manager service preserved' 14 ;;
    esac
}
# The bootstrap service may see the stock file while MPC sees an application overlay.
running_mpc() {
    for pid in $(pidof MPC 2>/dev/null); do
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        [ -d "$PROC/$pid" ] && [ "$(stat -c %u "$PROC/$pid")" = "$OWNER_UID" ] || continue
        [ -r "$PROC/$pid/exe" ] || continue
        printf '%s\n' "$PROC/$pid/exe"
        return 0
    done
    return 1
}
mounted() {
    while read -r device point rest; do
        [ "$(printf '%b' "$point")" != "$1" ] || return 0
    done < "$MOUNTS"
    return 1
}
sd_capable() {
    regular_source "$SOURCE/sd-install-capability" \
        && [ "$(wc -c < "$SOURCE/sd-install-capability")" = 23 ] \
        && [ "$(cat "$SOURCE/sd-install-capability")" = 'open-mpc-manager-sd-v1' ]
}
storage_config() {
    sd_capable || return 1
    sd_uuid=''
    regular_source "$SOURCE/installed-storage" || return 1
    { [ "$(stat -c %a "$SOURCE/installed-storage")" = 600 ] || [ "$SOURCE_EXFAT" = 1 ]; } \
        && [ "$(stat -c %h "$SOURCE/installed-storage")" = 1 ] || return 1
    [ "$(wc -c < "$SOURCE/installed-storage")" -le 94 ] || return 1
    {
        IFS= read -r header && IFS= read -r sd_uuid && ! IFS= read -r extra
    } < "$SOURCE/installed-storage" || return 1
    [ "$header" = 'open-mpc-manager-storage-v1' ] || return 1
    case "$sd_uuid" in ''|*[!0123456789abcdefABCDEF-]*) return 1 ;; esac
    [ "${#sd_uuid}" -le 64 ] \
        && [ "$(wc -c < "$SOURCE/installed-storage")" = "$((29 + ${#sd_uuid}))" ] || return 1
    sd_device=$(readlink -f "$UUIDS/$sd_uuid") || return 1
    [ -b "$sd_device" ] || return 1
    sd_number=''
    while read -r block point rest; do
        point=$(printf '%b' "$point")
        case "$point" in "$MEDIA"/?*) ;; *) continue ;; esac
        case "${point#"$MEDIA"/}" in */*) continue ;; esac
        [ -d "$point" ] && [ ! -L "$point" ] || continue
        mounted_device=$(readlink -f "$(printf '%b' "$block")") || continue
        [ "$mounted_device" = "$sd_device" ] || continue
        number=$(stat -c %d "$point") || return 1
        [ -z "$sd_number" ] || [ "$sd_number" = "$number" ] || return 1
        sd_number=$number
    done < "$MOUNTS"
    [ -n "$sd_number" ] || return 1
    for internal in / /data /nvme /storage "$INTERNAL" /media/az01-internal; do
        [ ! -e "$internal" ] || [ "$(stat -c %d "$internal")" != "$sd_number" ] || return 1
    done
}
source_mount() {
    case "$1" in
        "$MEDIA"/?*"$SD_SUFFIX")
            volume=${1%"$SD_SUFFIX"}
            case "${volume#"$MEDIA"/}" in */*) return 1 ;; esac
            storage_config && mounted "$volume" \
                && [ "$(stat -c %d "$volume")" = "$sd_number" ] \
                && [ "$(stat -c %d "$1")" = "$sd_number" ] || return 1
            matches=0
            while read -r block point rest; do
                point=$(printf '%b' "$point")
                case "$point" in "$MEDIA"/?*) ;; *) continue ;; esac
                [ -d "$point" ] || continue
                [ "$(stat -c %d "$point")" != "$sd_number" ] || matches=$((matches + 1))
            done < "$MOUNTS"
            [ "$matches" = 1 ]
            ;;
        "$MEDIA"/?*)
            case "${1#"$MEDIA"/}" in */*) return 1 ;; esac
            mounted "$1"
            ;;
        *) return 1 ;;
    esac
}
confirmed_mount() {
    [ -z "$confirmed_usb" ] || {
        [ "$usb" = "$confirmed_usb" ] && source_mount "$usb" \
            && [ "$(stat -c '%d:%i' "$usb"):$(stat -c '%d:%i' "$usb/.mpc-manager-id")" = "$confirmed_identity" ]
    }
}

confirmed_permission() {
    if [ -n "$preparation_directory" ]; then
        preparation_directory_valid && regular_source "$confirmed_permit" \
            && [ "$(stat -c %a "$confirmed_permit")" = 600 ] \
            && [ "$(stat -c %h "$confirmed_permit")" = 1 ] \
            && [ "$(stat -c '%d:%i' "$confirmed_permit")" = "$preparation_request_identity" ] \
            && [ "$(wc -c < "$confirmed_permit")" = 28 ] \
            && [ "$(cat "$confirmed_permit")" = 'open-mpc-manager-prepare-v1' ]
        return
    fi
    [ -z "$confirmed_usb" ] || {
        regular_source "$confirmed_permit" \
            && [ "$(stat -c %a "$confirmed_permit")" = 600 ] \
            && [ "$(stat -c %h "$confirmed_permit")" = 1 ] \
            && [ "$(wc -c < "$confirmed_permit")" = 30 ] \
            && [ "$(cat "$confirmed_permit")" = 'open-mpc-manager-confirmed-v1' ]
    }
}
release_permission() {
    [ -z "$preparation_directory" ] || return 0
    [ -z "$confirmed_permit" ] || {
        rm -f "$confirmed_permit"
        rmdir "${confirmed_permit%/permit}" 2>/dev/null || true
    }
}
preparation_directory_valid() {
    [ -d "$preparation_directory" ] && [ ! -L "$preparation_directory" ] \
        && [ "$(stat -c %u "$preparation_directory")" = "$OWNER_UID" ] \
        && [ "$(stat -c %a "$preparation_directory")" = 700 ] \
        && [ "$(stat -c '%d:%i' "$preparation_directory")" = "$preparation_directory_identity" ]
}
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$prepared_supervisor" = 1 ]; then
        [ "$preparation_published" = 1 ] || "$work/shortcut" --cancel-prepared "$work" >/dev/null 2>&1 || true
    else
        [ -z "$work" ] || rm -rf "$work"
    fi
    if [ -n "$preparation_directory_identity" ] && [ "$preparation_published" = 0 ] \
        && preparation_directory_valid; then
        (set -C; printf 'failed\n' > "$preparation_directory/failed") 2>/dev/null || true
    fi
    release_permission
    [ -z "$transport" ] || rm -rf "$transport"
    exit "$status"
}

confirmed_usb='' confirmed_identity='' confirmed_permit=''
preparation_directory='' preparation_directory_identity='' preparation_request_identity=''
work='' transport='' prepared_supervisor=0 preparation_published=0
case "$#:${1-}" in
    0:) ;;
    4:--confirmed|4:--prepare)
        confirmed_usb=$2 confirmed_identity=$3 confirmed_permit=$4 USB_WAIT=0
        if [ "$1" = --prepare ]; then
            case "$confirmed_permit" in "$RUN"/open-mpc-manager-preparation.[a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9]/request) ;; *) fail 'invalid preparation path' 10 ;; esac
            preparation_directory=${confirmed_permit%/request}
            [ -d "$preparation_directory" ] && [ ! -L "$preparation_directory" ] \
                && [ "$(stat -c %u "$preparation_directory")" = "$OWNER_UID" ] \
                && [ "$(stat -c %a "$preparation_directory")" = 700 ] || fail 'invalid preparation directory' 10
            preparation_directory_identity=$(stat -c '%d:%i' "$preparation_directory")
            trap cleanup EXIT
            trap 'exit 143' TERM
            trap 'exit 130' INT
            trap 'exit 129' HUP
            preparation_request_identity=$(stat -c '%d:%i' "$confirmed_permit") || fail 'preparation expired' 13
        fi
        case "$confirmed_usb" in "$MEDIA"/?*) ;; *) fail 'invalid confirmed USB' 10 ;; esac
        selected_volume=${confirmed_usb%"$SD_SUFFIX"}
        case "${selected_volume#"$MEDIA"/}" in */*) fail 'invalid confirmed USB' 10 ;; esac
        case "$confirmed_identity" in ''|*[!0-9:]*) fail 'invalid confirmed identity' 10 ;; esac
        [ "${#confirmed_identity}" -le 96 ] || fail 'invalid confirmed identity' 10
        if [ -z "$preparation_directory" ]; then
            case "$confirmed_permit" in "$RUN"/open-mpc-manager-approval.[a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9][a-zA-Z0-9]/permit) ;; *) fail 'invalid approval path' 10 ;; esac
            permit_directory=${confirmed_permit%/permit}
            [ -d "$permit_directory" ] && [ ! -L "$permit_directory" ] \
                && [ "$(stat -c %u "$permit_directory")" = "$OWNER_UID" ] \
                && [ "$(stat -c %a "$permit_directory")" = 700 ] || fail 'invalid approval directory' 10
            confirmed_permission || fail 'approval expired' 13
            trap release_permission EXIT
        else
            confirmed_permission || fail 'preparation expired' 13
        fi
        ;;
    *) fail 'invalid bootstrap arguments' 10 ;;
esac
[ "$(id -u)" = "$OWNER_UID" ] || fail 'unexpected bootstrap owner' 10
[ -d "$SOURCE" ] && [ ! -L "$SOURCE" ] || fail 'invalid source directory' 11
[ "$(CDPATH= cd "$SOURCE" && pwd -P)" = "$SOURCE" ] || fail 'source path contains a symlink' 11
[ "$(stat -c %u "$SOURCE")" = "$OWNER_UID" ] || fail 'source owner mismatch' 11
mode=$(stat -c %a "$SOURCE")
case "$mode" in ''|*[!0-7]*) fail 'invalid source permissions' 11 ;; esac
if [ "$MANIFEST" = runtime-aarch64.sha256 ] && [ "$(stat -f -c %t "$SOURCE")" = 2011bab0 ] \
    && [ "$(stat -c %d "$SOURCE")" = "$(stat -c %d "$INTERNAL")" ]; then SOURCE_EXFAT=1; fi
[ "${#mode}" -le 3 ] && { [ "$((0$mode & 0722))" = 448 ] || [ "$SOURCE_EXFAT" = 1 ]; } || fail 'unsafe source permissions' 11
if [ -n "$PAYLOAD" ]; then
    regular_source "$PAYLOAD/bootstrap.sh" && regular_source "$PAYLOAD/source-dispatcher-$ARCH" || fail 'invalid selected helper payload' 11
else
regular_source "$SOURCE/bootstrap.sh" && regular_source "$SOURCE/$MANIFEST" || fail 'invalid source file' 11
[ "$(wc -c < "$SOURCE/$MANIFEST")" -le 512 ] || fail 'manifest exceeds size bound' 11
bytes=0
exec 3< "$SOURCE/$MANIFEST"
for name in $FILES; do
    IFS= read -r entry <&3 || fail 'missing manifest entry' 11
    digest=${entry%%  *}
    [ "${#digest}" = 64 ] || fail 'invalid manifest checksum' 11
    case "$digest" in *[!0-9a-f]*) fail 'invalid manifest checksum' 11 ;; esac
    [ "$entry" = "$digest  $name" ] || fail 'invalid manifest filename' 11
    bytes=$((bytes + ${#entry} + 1))
done
! IFS= read -r entry <&3 || fail 'extra manifest entry' 11
exec 3<&-
[ "$(wc -c < "$SOURCE/$MANIFEST")" = "$bytes" ] || fail 'extra manifest content' 11
fi
firmware=$(running_mpc) || firmware=$MPC
actual=$(sha256sum "$firmware" 2>/dev/null) || fail 'firmware unavailable' 10
[ "${actual%% *}" = "$EXPECTED" ] || fail 'unsupported firmware' 10
[ -n "$PAYLOAD" ] || idle_session
[ -r "$MOUNTS" ] || fail 'mount inventory unavailable' 13
attempt=0
while :; do
    usb='' installed=''
    has_sd=0
    storage_config && has_sd=1
    for candidate in "$MEDIA"/*; do
        if [ "$has_sd" = 1 ] && [ -d "$candidate" ] \
            && [ "$(stat -c %d "$candidate")" = "$sd_number" ] \
            && [ -f "$candidate$SD_SUFFIX/.mpc-manager-id" ]; then
            [ -z "$installed" ] || fail 'ambiguous manager SD' 13
            installed="$candidate$SD_SUFFIX"
        fi
        [ -e "$candidate/.mpc-manager-id" ] || [ -L "$candidate/.mpc-manager-id" ] || continue
        [ -d "$candidate" ] && [ ! -L "$candidate" ] || fail 'invalid USB path' 11
        [ "$(CDPATH= cd "$candidate" && pwd -P)" = "$candidate" ] || fail 'USB path contains a symlink' 11
        mounted "$candidate" || continue
        [ -z "$usb" ] || fail 'ambiguous manager USB' 13
        usb=$candidate
    done
    if [ -n "$confirmed_usb" ]; then
        [ -n "$usb$installed" ] || fail 'manager USB unavailable' 13
        [ "$confirmed_usb" = "$usb" ] || [ "$confirmed_usb" = "$installed" ] || fail 'confirmed USB changed' 13
        usb=$confirmed_usb
    elif [ -z "$usb" ]; then
        usb=$installed
    fi
    if [ -n "$usb" ]; then
        source_mount "$usb" || fail 'manager source is not mounted' 13
        [ -d "$usb" ] && [ ! -L "$usb" ] \
            && [ "$(CDPATH= cd "$usb" && pwd -P)" = "$usb" ] || fail 'invalid USB path' 11
        marker="$usb/.mpc-manager-id"
        [ -f "$marker" ] && [ ! -L "$marker" ] || fail 'invalid USB marker' 11
        [ "$(wc -c < "$marker")" = 19 ] || fail 'invalid USB marker' 11
        actual=$(sha256sum "$marker")
        [ "${actual%% *}" = 3e89b070c92e11d262ed434d1829854525a07b468f81bd3db94e5cd993866e3d ] || fail 'invalid USB marker' 11
        break
    fi
    [ "$attempt" -lt "$USB_WAIT" ] || fail 'manager USB unavailable' 13
    sleep "$POLL_DELAY"
    attempt=$((attempt + 1))
done
confirmed_mount || fail 'confirmed USB changed' 13
confirmed_permission || fail 'approval expired' 13
if [ -n "$PAYLOAD" ]; then
    trap cleanup EXIT
    trap 'exit 143' TERM
    trap 'exit 130' INT
    trap 'exit 129' HUP
    transport=$(mktemp -d /tmp/pmu-helper-dispatch.XXXXXX) || fail 'dispatcher staging unavailable' 12
    chmod 700 "$transport"
    cp "$PAYLOAD/source-dispatcher-$ARCH" "$transport/dispatcher"
    expected_dispatcher=$(sha256sum "$PAYLOAD/source-dispatcher-$ARCH")
    actual_dispatcher=$(sha256sum "$transport/dispatcher")
    [ "${expected_dispatcher%% *}" = "${actual_dispatcher%% *}" ] || fail 'dispatcher RAM checksum mismatch' 12
    chmod 700 "$transport/dispatcher"
    mode=open
    [ -z "$preparation_directory" ] || mode=prepare
    work=$("$transport/dispatcher" "$usb" "$SOURCE" "$mode") || fail 'verified manager preparation failed' 12
    case "$work" in /tmp/mpc-usb-session.*) ;; *) work=''; fail 'invalid prepared manager cohort' 12 ;; esac
    [ -d "$work" ] && [ ! -L "$work" ] && [ "$(stat -c %u "$work")" = "$OWNER_UID" ] \
        && [ "$(stat -c %a "$work")" = 700 ] || { work=''; fail 'unsafe prepared manager cohort' 12; }
    prepared_supervisor=1
    confirmed_mount || fail 'confirmed USB changed' 13
    confirmed_permission || fail 'approval expired' 13
    if [ -n "$preparation_directory" ]; then
        (set -C; printf '%s\n' "$work" > "$preparation_directory/session") || fail 'preparation session unavailable' 12
        (set -C; printf '%s\n' "$work" > "$preparation_directory/ready") || fail 'preparation result unavailable' 12
    fi
    preparation_published=1
    remove_import_project
    exit 0
fi
runtime="$usb/autorun/runtime"
[ -d "$runtime" ] && [ ! -L "$runtime" ] || fail 'invalid USB runtime' 11
[ "$(CDPATH= cd "$runtime" && pwd -P)" = "$runtime" ] || fail 'runtime path contains a symlink' 11
for name in $FILES; do
    [ -f "$runtime/$name" ] && [ ! -L "$runtime/$name" ] || fail 'invalid runtime file' 11
done
(cd "$runtime" && sha256sum -c "$SOURCE/$MANIFEST" >/dev/null 2>&1) || fail 'runtime checksum mismatch' 11
idle_session

trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
work=$(mktemp -d "$RAM_TEMPLATE") || fail 'RAM staging unavailable' 12
[ "${work%??????}" = "${RAM_TEMPLATE%XXXXXX}" ] && [ -d "$work" ] && [ ! -L "$work" ] || {
    work=''
    fail 'unexpected RAM staging path' 12
}
chmod 700 "$work"
for name in $FILES; do
    case "$name" in */*) mkdir -p "$work/${name%/*}" ;; esac
    cp "$runtime/$name" "$work/$name"
done
(cd "$work" && sha256sum -c "$SOURCE/$MANIFEST" >/dev/null 2>&1) || fail 'RAM copy checksum mismatch' 12
if [ "$MANIFEST" = runtime-aarch64.sha256 ]; then
    for name in manager shortcut shortcut-hook.so; do mv "$work/aarch64/$name" "$work/$name"; done
    rmdir "$work/aarch64"
fi
chmod 700 "$work/manager" "$work/shortcut"
if sd_capable; then
    MPC_MANAGER_SD_HELPER=$SOURCE
    export MPC_MANAGER_SD_HELPER
fi
idle_session
confirmed_mount || fail 'confirmed USB changed' 13
confirmed_permission || fail 'approval expired' 13
if [ -n "$preparation_directory" ]; then
    (set -C; printf '%s\n' "$work" > "$preparation_directory/session") || fail 'preparation session unavailable' 12
    confirmed_permission || fail 'preparation expired' 13
    /bin/sh "$work/setup.sh" "$usb" "$work" --arm-only
    prepared_supervisor=1
    attempt=0
    while :; do
        confirmed_mount || fail 'confirmed USB changed' 13
        confirmed_permission || fail 'preparation expired' 13
        if [ -e "$work/ready" ] || [ -L "$work/ready" ]; then
            regular_source "$work/ready" && [ "$(stat -c %a "$work/ready")" = 600 ] \
                && [ "$(stat -c %h "$work/ready")" = 1 ] \
                && [ "$(wc -c < "$work/ready")" = 6 ] \
                && [ "$(cat "$work/ready")" = READY ] || fail 'invalid prepared readiness' 12
            break
        fi
        [ "$attempt" -lt "$READY_WAIT" ] || fail 'prepared supervisor unavailable' 12
        sleep "$POLL_DELAY"
        attempt=$((attempt + 1))
    done
    confirmed_mount || fail 'confirmed USB changed' 13
    confirmed_permission || fail 'preparation expired' 13
    (set -C; printf '%s\n' "$work" > "$preparation_directory/ready") || fail 'preparation result unavailable' 12
    preparation_published=1
    remove_import_project
    exit 0
fi
# setup.sh owns failure cleanup and transfers ownership to the independent USB supervisor.
trap - EXIT HUP INT TERM
release_permission
exec /bin/sh "$work/setup.sh" "$usb" "$work"

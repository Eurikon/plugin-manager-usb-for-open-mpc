#!/bin/sh
set -eu
cd "$(dirname "$0")"
output=${BUILD_DIR:-build}
mkdir -p "$output/startup"
architecture_flags=''
case "$(cc -dumpmachine)" in aarch64*) architecture_flags=-mno-outline-atomics ;; esac
cc $architecture_flags -std=c11 -O2 -Wall -Wextra -Werror -DPMU_REUSABLE=1 -DPMU_CONFIRMED=1 \
    -Isrc -fPIC -c src/startup/entry.c -o "$output"/startup/entry.o
c++ $architecture_flags -std=c++17 -O2 -Wall -Wextra -Werror -Wno-psabi -nostartfiles -shared -fPIC \
    -Isrc src/startup/hotplug.cpp src/startup/native_ui.cpp src/startup/panel_input.cpp \
    "$output"/startup/entry.o -pthread -ldl -Wl,-z,defs,-Bsymbolic-functions,--version-script=src/startup/helper.exports -o "$output"/startup/manager-entry.so
dynamic=$(readelf -d -W "$output"/startup/manager-entry.so)
case "$dynamic" in
    *"(INIT)"*|*"(FINI)"*|*"(INIT_ARRAY)"*|*"(FINI_ARRAY)"*|*"(INIT_ARRAYSZ)"*|*"(FINI_ARRAYSZ)"*)
        echo 'Helper must not contain load-time callbacks' >&2; exit 1 ;;
esac
symbols=$(readelf --dyn-syms -W "$output"/startup/manager-entry.so)
for name in pmu_start_manager pmu_stop_manager pmu_launch_confirmed pmu_prepare_manager pmu_helper_payload_v1; do
    printf '%s\n' "$symbols" | awk -v name="$name" '$7 != "UND" && $8 == name { found = 1 } END { exit !found }'
done

c++ $architecture_flags -std=c++17 -O2 -Wall -Wextra -Werror -Wno-psabi -nostartfiles -shared -fPIC \
    -Isrc -Isrc/startup src/startup/controller.cpp src/helper_loader.cpp src/version_store.cpp \
    src/checksum.cpp src/crypto.cpp -pthread -ldl -Wl,-z,defs,-Bsymbolic-functions,--version-script=src/startup/helper.exports -o "$output/startup/manager-loader.so"
dynamic=$(readelf -d -W "$output/startup/manager-loader.so")
case "$dynamic" in
    *"(INIT)"*|*"(FINI)"*|*"(INIT_ARRAY)"*|*"(FINI_ARRAY)"*|*"(INIT_ARRAYSZ)"*|*"(FINI_ARRAYSZ)"*)
        echo 'Controller must not contain load-time callbacks' >&2; exit 1 ;;
esac
for module in "$output/startup/manager-entry.so" "$output/startup/manager-loader.so"; do
    readelf --dyn-syms -W "$module" | awk '$4 == "FUNC" && $7 != "UND" && $5 != "LOCAL" && $8 !~ /^pmu_/ { invalid = 1 } END { exit invalid }'
done

cc -std=c11 -O2 -Wall -Wextra -Werror src/startup/bootstrap_note.c -o "$output/startup/bootstrap-note"

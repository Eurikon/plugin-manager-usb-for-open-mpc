#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build/startup
cc -std=c11 -O2 -Wall -Wextra -Werror -DPMU_REUSABLE=1 -DPMU_CONFIRMED=1 \
    -fPIC -c src/startup/entry.c -o build/startup/entry.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-psabi -nostartfiles -shared -fPIC \
    -Isrc src/startup/hotplug.cpp src/startup/native_ui.cpp src/startup/panel_input.cpp \
    build/startup/entry.o -pthread -ldl -Wl,-z,defs -o build/startup/manager-entry.so
dynamic=$(readelf -d -W build/startup/manager-entry.so)
case "$dynamic" in
    *"(INIT)"*|*"(FINI)"*|*"(INIT_ARRAY)"*|*"(FINI_ARRAY)"*|*"(INIT_ARRAYSZ)"*|*"(FINI_ARRAYSZ)"*)
        echo 'Helper must not contain load-time callbacks' >&2; exit 1 ;;
esac
symbols=$(readelf --dyn-syms -W build/startup/manager-entry.so)
for name in pmu_start_manager pmu_stop_manager pmu_launch_confirmed pmu_prepare_manager; do
    printf '%s\n' "$symbols" | awk -v name="$name" '$7 != "UND" && $8 == name { found = 1 } END { exit !found }'
done

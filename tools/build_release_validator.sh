#!/bin/sh
set -eu
baseline=$1
objects=$2
output=$3
project=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
set --
for unit in backend plugin_storage checksum crypto native_image version_store source_runtime update_worker update_bundle update_migration runtime_session migration_package session_handoff runtime_handoff runtime_worker; do
    test -f "$objects/$unit.o"
    set -- "$@" "$objects/$unit.o"
done
if [ -f "$objects/helper_controller_update.o" ]; then set -- "$@" "$objects/helper_controller_update.o"; fi
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$baseline/src" "$project/tests/release_update_test.cpp" "$@" -ldl -larchive -pthread -o "$output"

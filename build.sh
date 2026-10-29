#!/bin/sh
set -eu
cd "$(dirname "$0")"
output=${BUILD_DIR:-build}
mkdir -p "$output"
manager_version=$(sed -n 's/^#define MPC_MANAGER_VERSION "\([^"]*\)"$/\1/p' src/version.hpp)
test -n "$manager_version"
manager_development=1
if command -v git >/dev/null 2>&1 &&
   [ "$(git rev-parse --show-toplevel 2>/dev/null || :)" = "$(pwd -P)" ] &&
   [ "$(git describe --tags --exact-match HEAD 2>/dev/null || :)" = "v$manager_version" ] &&
   manager_changes=$(git status --porcelain --untracked-files=normal 2>/dev/null) &&
   [ -z "$manager_changes" ]; then
    manager_development=0
fi
manager_version_flag="-DMPC_MANAGER_DEVELOPMENT=$manager_development"
update_objects=''
for unit in checksum crypto native_image version_store source_runtime update_worker update_bundle update_migration runtime_session migration_package session_handoff runtime_handoff runtime_worker; do
    c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c "src/$unit.cpp" -o "$output/$unit.o"
    update_objects="$update_objects $output/$unit.o"
done
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc src/source_dispatcher.cpp "$output"/checksum.o "$output"/crypto.o "$output"/native_image.o "$output"/version_store.o "$output"/source_runtime.o "$output"/update_worker.o -ldl -pthread -o "$output"/source-dispatcher
strip "$output"/source-dispatcher
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/plugin_storage.cpp -o "$output"/plugin_storage.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -Isrc -c src/backend.cpp -o "$output"/backend.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/sd_install.cpp -o "$output"/sd_install.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/manager_update.cpp -o "$output"/manager_update.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags libpng) -c src/thumbnail.cpp -o "$output"/thumbnail.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -Isrc -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o "$output"/main.o
c++ "$output"/backend.o "$output"/plugin_storage.o "$output"/sd_install.o "$output"/manager_update.o "$output"/thumbnail.o "$output"/main.o $update_objects -ldl -o "$output"/manager $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive -Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic
strip "$output"/manager
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -Isrc tests/backend_test.cpp "$output"/backend.o "$output"/plugin_storage.o $update_objects -ldl -larchive -pthread -o "$output"/backend-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/architecture_test.cpp "$output"/backend.o "$output"/plugin_storage.o $update_objects -ldl -larchive -pthread -o "$output"/architecture-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Itests tests/sd_install_test.cpp "$output"/backend.o "$output"/plugin_storage.o "$output"/sd_install.o $update_objects -ldl -larchive -lcrypto -pthread -o "$output"/sd-install-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Itests tests/manager_update_test.cpp "$output"/backend.o "$output"/plugin_storage.o "$output"/manager_update.o $update_objects -ldl -larchive -pthread -o "$output"/manager-update-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/plugin_storage_test.cpp "$output"/backend.o "$output"/plugin_storage.o $update_objects -ldl -larchive -pthread -o "$output"/plugin-storage-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags libpng) tests/thumbnail_test.cpp "$output"/thumbnail.o -Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic -ldl -pthread -o "$output"/thumbnail-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc $(pkg-config --cflags libpng) tests/thumbnail_network_test.cpp "$output"/thumbnail.o -Wl,-Bstatic -lpng -ljpeg -lz -Wl,-Bdynamic -ldl -pthread -o "$output"/thumbnail-network-test
cp assets/font.ttf "$output"/font.ttf
cp licenses/TitilliumWeb.txt "$output"/font-license.txt
cp /usr/share/doc/nlohmann-json3-dev/copyright "$output"/json-license.txt

c++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC src/shortcut_hook.cpp -ldl -pthread -o "$output"/shortcut-hook.so
c++ -std=c++17 -O2 -Wall -Wextra -Werror src/shortcut.cpp -o "$output"/shortcut
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_state_test.cpp -o "$output"/shortcut-state-test
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC tests/shortcut_fake.c -o "$output"/shortcut-fake.so
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC tests/shortcut_lookup.c -o "$output"/shortcut-lookup.so
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -DADDIN_CALLS=shortcut_addin_first_reads -DADDIN_NEXT='"shortcut-addin-second.so"' tests/shortcut_addin.c -ldl -o "$output"/shortcut-addin-first.so
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -DADDIN_CALLS=shortcut_addin_second_reads -DADDIN_NEXT='"shortcut-hook.so"' tests/shortcut_addin.c -ldl -o "$output"/shortcut-addin-second.so
cc -std=c11 -O2 -Wall -Wextra -Werror tests/shortcut_hook_test.c -L"$output" -l:shortcut-fake.so -Wl,-rpath,/tmp/shortcut-test -ldl -o "$output"/shortcut-hook-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_integration.cpp -o "$output"/shortcut-integration
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_environment_test.cpp -o "$output"/shortcut-environment-test
if [ "${BUILD_HELPER:-1}" = 1 ]; then BUILD_DIR="$output" sh build_helper.sh; fi

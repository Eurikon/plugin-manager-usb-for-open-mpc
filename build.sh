#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
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
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -Isrc -c src/backend.cpp -o build/backend.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/sd_install.cpp -o build/sd_install.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/manager_update.cpp -o build/manager_update.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o build/main.o
c++ build/backend.o build/sd_install.o build/manager_update.o build/main.o -o build/manager $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive
strip build/manager
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$manager_version_flag" -Isrc tests/backend_test.cpp build/backend.o -larchive -pthread -o build/backend-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Itests tests/sd_install_test.cpp build/backend.o build/sd_install.o -larchive -pthread -o build/sd-install-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Itests tests/manager_update_test.cpp build/backend.o build/manager_update.o -larchive -pthread -o build/manager-update-test
cp assets/font.ttf build/font.ttf
cp licenses/TitilliumWeb.txt build/font-license.txt
cp /usr/share/doc/nlohmann-json3-dev/copyright build/json-license.txt

c++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC src/shortcut_hook.cpp -ldl -pthread -o build/shortcut-hook.so
c++ -std=c++17 -O2 -Wall -Wextra -Werror src/shortcut.cpp -o build/shortcut
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_state_test.cpp -o build/shortcut-state-test
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC tests/shortcut_fake.c -o build/shortcut-fake.so
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -DADDIN_CALLS=shortcut_addin_first_reads -DADDIN_NEXT='"shortcut-addin-second.so"' tests/shortcut_addin.c -ldl -o build/shortcut-addin-first.so
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -DADDIN_CALLS=shortcut_addin_second_reads -DADDIN_NEXT='"shortcut-hook.so"' tests/shortcut_addin.c -ldl -o build/shortcut-addin-second.so
cc -std=c11 -O2 -Wall -Wextra -Werror tests/shortcut_hook_test.c -Lbuild -l:shortcut-fake.so -Wl,-rpath,/tmp/shortcut-test -ldl -o build/shortcut-hook-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_integration.cpp -o build/shortcut-integration
sh build_helper.sh

#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -c src/backend.cpp -o build/backend.o
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I/opt/sdl/include/SDL2 $(pkg-config --cflags freetype2) -c src/main.cpp -o build/main.o
c++ build/backend.o build/main.o -o build/manager $(/opt/sdl/bin/sdl2-config --static-libs) $(pkg-config --libs freetype2) -larchive
strip build/manager
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc tests/backend_test.cpp build/backend.o -larchive -pthread -o build/backend-test
cp assets/font.ttf build/font.ttf
cp licenses/TitilliumWeb.txt build/font-license.txt
cp /usr/share/doc/nlohmann-json3-dev/copyright build/json-license.txt

c++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC src/shortcut_hook.cpp -ldl -pthread -o build/shortcut-hook.so
c++ -std=c++17 -O2 -Wall -Wextra -Werror src/shortcut.cpp -o build/shortcut
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_state_test.cpp -o build/shortcut-state-test
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC tests/shortcut_fake.c -o build/shortcut-fake.so
cc -std=c11 -O2 -Wall -Wextra -Werror tests/shortcut_hook_test.c -Lbuild -l:shortcut-fake.so -Wl,-rpath,/tmp/shortcut-test -o build/shortcut-hook-test
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/shortcut_integration.cpp -o build/shortcut-integration

#!/bin/bash
# Build (and optionally convert snapshots for) the host simulator.
#   ./hostsim/build.sh            -> builds ./hostsim/sim
#   ./hostsim/build.sh snap       -> builds, runs, converts raw -> PNG
set -e
cd "$(dirname "$0")/.."   # lvgl-demo/

CFLAGS="-O1 -DLV_CONF_INCLUDE_SIMPLE -Ihostsim -Ilib -Ilib/lvgl -Isrc -Wno-unused-function"
LDFLAGS="-L/opt/homebrew/lib -lSDL2 -lm"
SDL="-I/opt/homebrew/include/SDL2 -I/opt/homebrew/include"

mkdir -p hostsim/build
rm -rf hostsim/build/*

cat > hostsim/compile_one.sh <<'EOS'
#!/bin/bash
# usage: compile_one.sh <src.c> <out.o>
exec clang -c "$1" -O1 -DLV_CONF_INCLUDE_SIMPLE -Ihostsim -Ilib -Ilib/lvgl -Isrc \
    -Wno-unused-function -o "$2"
EOS
chmod +x hostsim/compile_one.sh

echo "compiling LVGL (C)..."
find lib/lvgl/src -name '*.c' | while read -r f; do
  echo "$f hostsim/build/$(echo "$f" | tr '/' '_').o"
done > hostsim/build/manifest.txt

xargs -P 12 -n 2 hostsim/compile_one.sh < hostsim/build/manifest.txt

echo "compiling C++..."
clang++ -std=c++17 $CFLAGS $SDL -c src/dashboard_ui.cpp -o hostsim/build/dashboard_ui.o
clang++ -std=c++17 $CFLAGS $SDL -c hostsim/main.cpp -o hostsim/build/main.o

echo "linking..."
clang++ hostsim/build/*.o -o sim $LDFLAGS
echo "built ./sim"

if [ "${1:-}" = "snap" ]; then
    ./sim
    python3 hostsim/raw2png.py hostsim/out_tab1.raw hostsim/out_tab1.png
    python3 hostsim/raw2png.py hostsim/out_tab2.raw hostsim/out_tab2.png
fi

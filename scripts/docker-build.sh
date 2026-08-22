#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 build container. Do not run on a host with a
# newer glibc — the artifacts would not load on the Move (glibc 2.35).
set -euo pipefail
TARGET="${1:-all}"

# ---- Config contract: movy_config / chain_params / C table / chain UI must
# all agree, and enum options must stay distinct in movy's 5-char readout.
echo "=== config contract ==="
python3 tools/check_config.py

# ---- The modified core must be bit-identical to upstream with Ping Pong off.
# Fetch the pristine core and render the same program through both.
echo "=== core equivalence vs pristine upstream ==="
rm -rf build-native/pristine && mkdir -p build-native/pristine
if git clone -q --depth 1 https://github.com/dusk-audio/dusk-audio-plugins \
        build-native/dusk 2>/dev/null; then
    cp build-native/dusk/plugins/tape-echo/core/TapeEchoDSP.hpp \
       build-native/dusk/plugins/tape-echo/core/TapeEchoDSP.cpp build-native/pristine/
    g++ -O2 -std=c++17 tests/core_equivalence.cpp build-native/pristine/TapeEchoDSP.cpp \
        -Ibuild-native/pristine -Isrc/ported/shared-dpf/dsp \
        -Ibuild-native/dusk/plugins/tape-echo/dpf-plugin -o build-native/eq_pristine
    g++ -O2 -std=c++17 tests/core_equivalence.cpp src/ported/core/TapeEchoDSP.cpp \
        -Isrc/ported/core -Isrc/ported/shared-dpf/dsp -Isrc/ported/dpf-plugin \
        -o build-native/eq_ours
    A=$(./build-native/eq_pristine); B=$(./build-native/eq_ours)
    echo "  pristine=$A  ours=$B"
    [ "$A" = "$B" ] || { echo "FAIL: the core diverges from upstream with Ping Pong off"; exit 1; }
    echo "  ok: identical with Ping Pong off"
else
    echo "  (skipped: upstream not reachable)"
fi

# ---- Native DSP tests: compile and RUN in-container before cross-compiling.
# A red test here fails the whole build.
echo "=== native DSP tests ==="
mkdir -p build-native
g++ -O2 -std=c++17 -Wall \
    tests/test_dsp.cpp src/ported/core/TapeEchoDSP.cpp \
    -Isrc -Isrc/host -Isrc/ported/core -Isrc/ported/shared-dpf/dsp \
    -o build-native/te2_test -lm -lpthread
./build-native/te2_test

# Native loadtest against a natively built tape-echo2.so: full API-surface check.
echo "=== native loadtest ==="
g++ -O2 -std=c++17 -Wall -shared -fPIC \
    src/dsp/tape_echo_plugin.cpp src/ported/core/TapeEchoDSP.cpp \
    -Isrc -Isrc/host -Isrc/ported/core -Isrc/ported/shared-dpf/dsp \
    -o build-native/tape-echo2.so -lm -lpthread
g++ -O2 -std=c++17 -Wall tools/loadtest.cpp \
    -Isrc -Isrc/host \
    -o build-native/te2_loadtest -ldl -lm
./build-native/te2_loadtest build-native/tape-echo2.so src/module.json

cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --target "$TARGET" -j"$(nproc)"

# ---- Package for the Module Store ----
rm -rf dist/tape-echo2
mkdir -p dist/tape-echo2
cp build/tape-echo2.so   dist/tape-echo2/
cp src/module.json       dist/tape-echo2/
cp src/movy_config.json  dist/tape-echo2/
cp src/ui_chain.js       dist/tape-echo2/
cp src/help.json         dist/tape-echo2/
cp src/web_ui.html       dist/tape-echo2/
cp LICENSE               dist/tape-echo2/
(cd dist && tar -czf tape-echo2-module.tar.gz tape-echo2/)
echo "Tarball: dist/tape-echo2-module.tar.gz"

echo; echo "=== Build output ==="
find build -maxdepth 1 -type f \( -name "*.so" -o -name "te2_*" \) \
    -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;

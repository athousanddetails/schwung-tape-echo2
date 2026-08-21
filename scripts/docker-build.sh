#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 build container. Do not run on a host with a
# newer glibc — the artifacts would not load on the Move (glibc 2.35).
set -euo pipefail
TARGET="${1:-all}"

# ---- Config contract: movy_config / chain_params / C table / chain UI must
# all agree, and enum options must stay distinct in movy's 5-char readout.
echo "=== config contract ==="
python3 tools/check_config.py

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

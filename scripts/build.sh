#!/usr/bin/env bash
# Build Tape Echo 2 for Ableton Move (aarch64) — from the Mac.
#
# The Mac has no toolchain and no Docker (deliberately). This script:
#   1. rsyncs the source up to the VPS (~/schwung-dev/schwung-tapeecho2)
#   2. builds there inside Docker (ubuntu:22.04 = glibc 2.35, matching Move)
#   3. rsyncs build/ and dist/ back down
#
#   ./scripts/build.sh [cmake-target]        (default: all)
set -euo pipefail

VPS="vps"
REMOTE_DIR="schwung-dev/schwung-tapeecho2"
TARGET="${1:-all}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== 1/3 rsync source -> $VPS:$REMOTE_DIR"
ssh "$VPS" "mkdir -p $REMOTE_DIR"
rsync -az --delete \
    --exclude .git --exclude build/ --exclude build-native/ \
    --exclude dist/ --exclude .DS_Store \
    "$SRC/" "$VPS:$REMOTE_DIR/"

echo "=== 2/3 docker build on $VPS"
ssh "$VPS" "cd $REMOTE_DIR && \
    docker image inspect schwung-te2-builder >/dev/null 2>&1 || \
        docker build -t schwung-te2-builder -f scripts/Dockerfile scripts/ && \
    docker run --rm -v \$PWD:/build -u \$(id -u):\$(id -g) -w /build \
        schwung-te2-builder ./scripts/docker-build.sh $TARGET"

echo "=== 3/3 rsync artifacts back"
rsync -az "$VPS:$REMOTE_DIR/build/" "$SRC/build/"
rsync -az "$VPS:$REMOTE_DIR/dist/" "$SRC/dist/" 2>/dev/null || true

echo; echo "=== Artifacts ==="
ls -la "$SRC/build/" | grep -E "\.so|loadtest|te2_" || true

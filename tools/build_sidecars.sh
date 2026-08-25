#!/bin/bash
# Build lfp_build and generate .lfp sidecars for an archive.
#
#   docker run --rm -m 6g -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
#       dschirp-dev bash /work/ionozond/tools/build_sidecars.sh [archive-dir] [--force]
set -eu
SRC=/work/ionozond
BUILD=/tmp/build-lfp
TARGET="${1:-/data}"
shift || true

mkdir -p "$BUILD"
cd "$BUILD"
qmake "$SRC/lfp_build.pro" >/dev/null
make -j"$(nproc)" >/tmp/make-lfp.log 2>&1 || { echo "BUILD FAILED"; tail -25 /tmp/make-lfp.log; exit 1; }
echo "built $BUILD/lfp_build"
echo

export QT_QPA_PLATFORM=offscreen
"$BUILD/lfp_build" "$TARGET" "$@"

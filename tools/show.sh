#!/bin/bash
# Build and run the QRxIonogram widget harness, offscreen.
#   docker run --rm -m 6g -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
#       dschirp-dev bash /work/ionozond/tools/show.sh [capture] [out.png]
set -eu
SRC=/work/ionozond
BUILD=/tmp/build
mkdir -p "$BUILD"
CAP=${1:-/data/cyprus1_20191023_071510.lfs}
OUT=${2:-$SRC/tests/out/widget_ionogram.png}

QT_DIR=/usr/include/x86_64-linux-gnu/qt5
QT_INC="-I$QT_DIR -I$QT_DIR/QtCore -I$QT_DIR/QtGui -I$QT_DIR/QtWidgets -I/usr/include/qwt"
QT_LIB="-lQt5Core -lQt5Gui -lQt5Widgets -lqwt-qt5"

/usr/lib/qt5/bin/moc $QT_INC "$SRC/src/common.h"      -o "$BUILD/moc_common.cpp"
/usr/lib/qt5/bin/moc $QT_INC "$SRC/src/qrxionogram.h" -o "$BUILD/moc_qrxionogram.cpp"

g++ -std=c++11 -O2 -fPIC -Wall $QT_INC -o "$BUILD/show_ionogram" \
    "$SRC/tools/show_ionogram.cpp" \
    "$SRC/src/common.cpp" "$BUILD/moc_common.cpp" \
    "$SRC/src/qrxionogram.cpp" "$BUILD/moc_qrxionogram.cpp" \
    "$SRC/src/rasterdata.cpp" "$SRC/src/lfs_header.cpp" "$SRC/src/igmath.cpp" \
    "$SRC/src/qigcolormap.cpp" "$SRC/src/qigcolormap_tables.cpp" \
    $QT_LIB -lfftw3 -lm

export QT_QPA_PLATFORM=offscreen
"$BUILD/show_ionogram" "$CAP" "$OUT" "${3:-760}" "${4:-420}"

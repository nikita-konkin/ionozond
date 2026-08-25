#!/bin/bash
# Build and run the reconstruction's tests inside the dev container.
#
#   docker run --rm -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
#       dschirp-dev bash /work/ionozond/tests/run_tests.sh
set -u
SRC=/work/ionozond
BUILD=/tmp/build
CAP=${CAP:-/data/cyprus1_20191023_071510.lfs}
N=${N:-16384}
NSPEC=${NSPEC:-8}
mkdir -p "$BUILD"
rc=0

QT_DIR=/usr/include/x86_64-linux-gnu/qt5
QT_INC="-I$QT_DIR -I$QT_DIR/QtCore -I$QT_DIR/QtGui -I$QT_DIR/QtWidgets -I/usr/include/qwt"
QT_LIB="-lQt5Core -lQt5Gui -lQt5Widgets -lqwt-qt5"
CXXFLAGS="-std=c++11 -Wall -Wextra -O2 -fPIC"

echo "=== test_igmath (differential vs original binary) ==="
g++ $CXXFLAGS -o "$BUILD/test_igmath" \
    "$SRC/tests/test_igmath.cpp" "$SRC/src/igmath.cpp" -lfftw3 -lm || exit 1
"$BUILD/test_igmath" || rc=1

echo
echo "=== test_lfs_header ==="
g++ $CXXFLAGS -o "$BUILD/test_lfs_header" \
    "$SRC/tests/test_lfs_header.cpp" "$SRC/src/lfs_header.cpp" || exit 1
"$BUILD/test_lfs_header" /data/*.lfs || rc=1

echo
echo "=== test_spectrum (C++) ==="
g++ $CXXFLAGS -o "$BUILD/test_spectrum" \
    "$SRC/tests/test_spectrum.cpp" "$SRC/src/lfs_header.cpp" "$SRC/src/igmath.cpp" \
    -lfftw3 -lm || exit 1
"$BUILD/test_spectrum" "$CAP" "$N" "$NSPEC" "$BUILD/cpp_spec.bin" || rc=1

echo
echo "=== spectrum_oracle (NumPy) ==="
python3 "$SRC/python/spectrum_oracle.py" "$CAP" "$N" "$NSPEC" "$BUILD/cpp_spec.bin" || rc=1

echo
echo "=== test_configwriter (byte-diff vs original binary output) ==="
g++ $CXXFLAGS $QT_INC -o "$BUILD/test_configwriter" \
    "$SRC/tests/test_configwriter.cpp" "$SRC/src/configwriter.cpp" $QT_LIB || exit 1
"$BUILD/test_configwriter" "$SRC/tests/golden" || rc=1

echo
echo "=== test_schedule ==="
g++ $CXXFLAGS $QT_INC -o "$BUILD/test_schedule" \
    "$SRC/tests/test_schedule.cpp" "$SRC/src/schedule.cpp" $QT_LIB || exit 1
"$BUILD/test_schedule" || rc=1

echo
echo "=== test_axes (vs the original's displayed ranges) ==="
# common.cpp defines DigitalClock (a QObject), so it needs moc
/usr/lib/qt5/bin/moc $QT_INC "$SRC/src/common.h" -o "$BUILD/moc_common.cpp"
g++ $CXXFLAGS $QT_INC -o "$BUILD/test_axes" \
    "$SRC/tests/test_axes.cpp" "$SRC/src/common.cpp" "$BUILD/moc_common.cpp" \
    "$SRC/src/lfs_header.cpp" \
    "$SRC/src/qigcolormap.cpp" "$SRC/src/qigcolormap_tables.cpp" \
    $QT_LIB -lm || exit 1
"$BUILD/test_axes" "$CAP" || rc=1

echo
echo "=== test_cleaning (ionogram cleaning stages) ==="
g++ $CXXFLAGS $QT_INC -o "$BUILD/test_cleaning" \
    "$SRC/tests/test_cleaning.cpp" "$SRC/src/iganalytics.cpp" "$SRC/src/igmath.cpp" \
    $QT_LIB -lfftw3 -lm || exit 1
"$BUILD/test_cleaning" || rc=1

echo
echo "=== test_lfp (sidecar round-trip) ==="
g++ $CXXFLAGS $QT_INC -o "$BUILD/test_lfp" \
    "$SRC/tests/test_lfp.cpp" "$SRC/src/lfpfile.cpp" $QT_LIB || exit 1
"$BUILD/test_lfp" || rc=1

echo
[ $rc -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $rc

#!/bin/bash
#
# Build and set up the console to run natively on the sounder host.
#
#   bash tools/build-native.sh
#   bash tools/build-native.sh --run
#
# Why not the container
# ---------------------
# The Docker image exists so the console can be driven from a browser on a
# machine with no Qt -- useful for development, and on Windows. It cannot run
# the sounder: it has no UHD, no python3-uhd, and mounts the archive
# read-only. A console inside it can display captures and nothing else.
#
# On the sounder host itself none of that indirection helps. Built natively,
# the console sees the real filesystem, can launch tools/sounder.sh, and the
# paths in its dialogs are the paths that exist.
#
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$HOME/.cache/ionozond-build}"
ARCHIVE="${ARCHIVE:-$HOME/ionograms}"
CONFIG_DIR="$HOME/.config/dsChirp"
CHIRP_CONFIG="${CHIRP_CONFIG:-$HOME/chirp_config.py}"

PACKAGES="build-essential qtbase5-dev qtbase5-dev-tools qttools5-dev-tools
          libqwt-qt5-dev libfftw3-dev"

echo "=================================================================="
echo " building the console natively"
echo "=================================================================="
echo "  source   $HERE"
echo "  build    $BUILD"
echo "  archive  $ARCHIVE"

# ---- dependencies --------------------------------------------------------
missing=""
for pkg in $PACKAGES; do
    dpkg -s "$pkg" >/dev/null 2>&1 || missing="$missing $pkg"
done
if [ -n "$missing" ]; then
    echo
    echo "--- missing packages ------------------------------------------------"
    echo "  $missing"
    echo
    echo "  sudo apt-get install -y$missing"
    echo
    echo "Install those and run this again."
    exit 1
fi

# ---- build ---------------------------------------------------------------
echo
echo "--- compiling --------------------------------------------------------"
mkdir -p "$BUILD"
cd "$BUILD"
if ! qmake "$HERE/ionozond.pro" >/tmp/qmake-native.log 2>&1; then
    echo "  qmake failed:"; sed 's/^/    /' /tmp/qmake-native.log; exit 1
fi
if ! make -j"$(nproc)" >/tmp/make-native.log 2>&1; then
    echo "  BUILD FAILED -- last 30 lines:"
    tail -30 /tmp/make-native.log | sed 's/^/    /'
    echo
    echo "  full log: /tmp/make-native.log"
    exit 1
fi
echo "  built $BUILD/ionozond"

# ---- settings ------------------------------------------------------------
# Seed the console's settings the first time, with paths that exist on this
# host. It reads ~/.config/dsChirp, not the container's /root/.config.
echo
echo "--- settings ---------------------------------------------------------"
mkdir -p "$CONFIG_DIR" "$ARCHIVE"
if [ ! -f "$CONFIG_DIR/config.ini" ]; then
    cp "$HERE/tests/fixtures/config.ini" "$CONFIG_DIR/config.ini"
    cp "$HERE/tests/fixtures/schedule.ini" "$CONFIG_DIR/schedule.ini"
    echo "  seeded $CONFIG_DIR from tests/fixtures"
fi

# Point it at this host's paths rather than the fixture's container ones.
sed -i "s|^data_dir=.*|data_dir=$ARCHIVE/|" "$CONFIG_DIR/config.ini"
sed -i "s|^sound_app=.*|sound_app=$HERE/tools/sounder.sh|" "$CONFIG_DIR/config.ini"
sed -i "s|^config_file=.*|config_file=$CHIRP_CONFIG|" "$CONFIG_DIR/config.ini"
grep -E "^(data_dir|sound_app|config_file)=" "$CONFIG_DIR/config.ini" | sed 's/^/  /'

chmod +x "$HERE/tools/sounder.sh" 2>/dev/null || true

echo
echo "=================================================================="
echo " run it:"
echo "     $BUILD/ionozond"
echo
echo " Press START and the console launches the sounder itself. It needs to"
echo " reach the radio and to write to $ARCHIVE, so run it as the"
echo " user that owns both -- not under sudo."
echo
echo " If UHD cannot raise its thread priority, that user is not yet in the"
echo " usrp group for this login session. 12-host-tuning.sh added it; log"
echo " out and back in once."
echo "=================================================================="

if [ "${1:-}" = "--run" ]; then
    exec "$BUILD/ionozond"
fi

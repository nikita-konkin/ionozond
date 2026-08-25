#!/bin/bash
#
# Interactive GUI runner (inside the container).
#
# Starts a virtual display, a VNC server and noVNC, then launches one of the
# programs. Point a browser at http://localhost:6080/vnc.html to drive it.
#
#   run-gui.sh [mode] [args...]
#
# Modes:
#   app        the reconstructed dsChirp          (default)
#   original   the shipped binary, for comparison
#   both       both side by side on one desktop
#   viewer     single-capture viewer: viewer <file.lfs>
#
# Environment:
#   DATA_DIR   archive root to point the app at   (default /data)
#   GEOMETRY   virtual screen size                (default 1600x1000)
set -u

MODE="${1:-app}"
shift || true

SRC=/work/ionozond
BUILD=/tmp/build-app
DATA_DIR="${DATA_DIR:-/data}"
GEOMETRY="${GEOMETRY:-1600x1000}"

export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

# ---------------------------------------------------------------- build
echo "=== building ==="
mkdir -p "$BUILD"
cd "$BUILD"
qmake "$SRC/ionozond.pro" >/dev/null
if ! make -j"$(nproc)" >/tmp/make.log 2>&1; then
    echo "BUILD FAILED:"; tail -30 /tmp/make.log; exit 1
fi
echo "build ok -> $BUILD/ionozond"

mkdir -p "$BUILD/viewer"
cd "$BUILD/viewer"
qmake "$SRC/viewer.pro" >/dev/null
if ! make -j"$(nproc)" >/tmp/make-viewer.log 2>&1; then
    echo "VIEWER BUILD FAILED:"; tail -30 /tmp/make-viewer.log; exit 1
fi
cp "$BUILD/viewer/dsChirp_viewer" "$BUILD/dsChirp_viewer"
echo "build ok -> $BUILD/dsChirp_viewer"
cd "$BUILD"

# ---------------------------------------------------------------- settings
echo "=== settings ==="
mkdir -p /root/.config/dsChirp /tmp/out
if [ ! -f /root/.config/dsChirp/config.ini ]; then
    cp "$SRC/tests/fixtures/config.ini"   /root/.config/dsChirp/
    cp "$SRC/tests/fixtures/schedule.ini" /root/.config/dsChirp/
fi
: > /tmp/out/chirp_config.py

# Point data_dir at whatever archive was mounted, and make sure it exists.
if [ -d "$DATA_DIR" ]; then
    sed -i "s|^data_dir=.*|data_dir=${DATA_DIR}/|" /root/.config/dsChirp/config.ini
    echo "data_dir = ${DATA_DIR}/"
fi
mkdir -p "${DATA_DIR}/logs" 2>/dev/null || true

# If the archive has captures lying loose rather than in <yyyy.MM.dd>/ dirs,
# link them into the layout the app expects so they show up on start.
shopt -s nullglob
for f in "$DATA_DIR"/*.lfs; do
    base=$(basename "$f")                       # station_YYYYMMDD_HHMMSS.lfs
    stamp=$(echo "$base" | sed -n 's/.*_\([0-9]\{8\}\)_[0-9]\{6\}\.lfs/\1/p')
    [ -n "$stamp" ] || continue
    day="${stamp:0:4}.${stamp:4:2}.${stamp:6:2}"
    mkdir -p "/tmp/igs/$day" 2>/dev/null || true
    ln -sf "$f" "/tmp/igs/$day/$base" 2>/dev/null || true
done
if [ -d /tmp/igs ] && [ -n "$(ls -A /tmp/igs 2>/dev/null)" ]; then
    echo "loose captures linked into /tmp/igs; using that as data_dir"
    sed -i "s|^data_dir=.*|data_dir=/tmp/igs/|" /root/.config/dsChirp/config.ini
    mkdir -p /tmp/igs/logs
fi
shopt -u nullglob

# ---------------------------------------------------------------- display
echo "=== display ==="
Xvfb :99 -screen 0 "${GEOMETRY}x24" >/tmp/xvfb.log 2>&1 &
sleep 2
openbox >/tmp/openbox.log 2>&1 &            # window manager: move/resize windows
sleep 1
x11vnc -display :99 -forever -shared -nopw -quiet -rfbport 5900 >/tmp/x11vnc.log 2>&1 &
sleep 1
websockify --web=/usr/share/novnc 6080 localhost:5900 >/tmp/novnc.log 2>&1 &
sleep 1

echo
echo "================================================================"
echo "  Open  http://localhost:6080/vnc.html   and press Connect"
echo "================================================================"
echo

# ---------------------------------------------------------------- run
case "$MODE" in
  app)
    echo "launching the reconstruction"
    "$BUILD/ionozond" 2>&1 | sed 's/^/[app] /' &
    ;;
  original)
    echo "launching the ORIGINAL binary"
    /work/dsChirp/bin/dsChirp 2>&1 | sed 's/^/[orig] /' &
    ;;
  both)
    echo "launching both (original on the left, reconstruction on the right)"
    /work/dsChirp/bin/dsChirp 2>&1 | sed 's/^/[orig] /' &
    sleep 6
    "$BUILD/ionozond" 2>&1 | sed 's/^/[app] /' &
    ;;
  viewer)
    CAP="${1:-}"
    if [ -z "$CAP" ]; then
        echo "usage: run-gui.sh viewer <file.lfs>"; exit 2
    fi
    echo "opening $CAP"
    "$BUILD/dsChirp_viewer" "$CAP" 2>&1 | sed 's/^/[viewer] /' &
    ;;
  *)
    echo "unknown mode: $MODE"; exit 2
    ;;
esac

echo
echo "Ctrl-C here (or stopping the container) shuts everything down."
wait

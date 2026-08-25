#!/bin/bash
# Build the reconstruction, run it under Xvfb with the same fixture settings
# used for the oracle, and screenshot it for side-by-side comparison.
set -u
SRC=/work/ionozond
OUTDIR=$SRC/tests/out
FIX=$SRC/tests/fixtures
BUILD=/tmp/build-app

mkdir -p "$BUILD" "$OUTDIR"
cd "$BUILD"
qmake "$SRC/ionozond.pro" >/dev/null
make -j"$(nproc)" >/tmp/make.log 2>&1 || { echo "BUILD FAILED"; tail -20 /tmp/make.log; exit 1; }
echo "### build ok"

rm -rf /root/.config/dsChirp /tmp/igs /tmp/out
mkdir -p /root/.config/dsChirp /tmp/igs/logs /tmp/out
cp "$FIX/config.ini" "$FIX/schedule.ini" /root/.config/dsChirp/
: > /tmp/out/chirp_config.py

# Lay a real capture out the way the app expects to find it:
#   <data_dir>/<yyyy.MM.dd>/<station>_<yyyyMMdd>_<hhmmss>.lfs
mkdir -p /tmp/igs/2019.10.23
for f in /data/cyprus1_*.lfs; do
  [ -e "$f" ] && ln -sf "$f" "/tmp/igs/2019.10.23/$(basename "$f")"
done

export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

Xvfb :99 -screen 0 1600x1000x24 >/tmp/xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2
"$BUILD/ionozond" >/tmp/app.log 2>&1 &
APP_PID=$!
sleep 5

WID=$(xdotool search --name "." 2>/dev/null | tail -1)
if [ -n "${WID:-}" ]; then
  xdotool windowsize "$WID" 1600 1000 2>/dev/null || true
  xdotool windowmove "$WID" 0 0 2>/dev/null || true
  sleep 2
fi
import -window root "$OUTDIR/rebuild_main.png" 2>/dev/null
echo "### screenshot: $OUTDIR/rebuild_main.png"

# Optional click sequence, same form as drive_oracle.sh: "x,y[:label]"
i=0
for spec in "$@"; do
  i=$((i+1))
  xy="${spec%%:*}"; label="${spec##*:}"
  [ "$label" = "$xy" ] && label="step$i"
  x="${xy%%,*}"; y="${xy##*,}"
  echo "### click $i at $x,$y ($label)"
  xdotool mousemove "$x" "$y" click 1
  sleep 3
  import -window root "$OUTDIR/rebuild_${label}.png" 2>/dev/null
done

echo "### app stderr:"; cat /tmp/app.log

kill $APP_PID 2>/dev/null; kill $XVFB_PID 2>/dev/null; wait 2>/dev/null
exit 0

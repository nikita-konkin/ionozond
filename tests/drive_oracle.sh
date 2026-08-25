#!/bin/bash
# Drive the ORIGINAL dsChirp under Xvfb and collect what it produces.
#
#   drive_oracle.sh [x,y[:label] ...]
#
# Each argument is a click; a screenshot is saved after each one. Example:
#   drive_oracle.sh 1493,131:params 1290,760:ok 1425,131:start
set -u
OUTDIR=/work/ionozond/tests/out
FIX=/work/ionozond/tests/fixtures

rm -rf /root/.config/dsChirp /tmp/igs /tmp/out
mkdir -p /root/.config/dsChirp /tmp/igs/logs /tmp/out "$OUTDIR"
cp "$FIX/config.ini" "$FIX/schedule.ini" /root/.config/dsChirp/

# ParametersDialog::on_btbOkCancel_accepted() refuses to accept unless the
# configured paths exist, so make them exist before driving the dialog.
: > /tmp/out/chirp_config.py

export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

Xvfb :99 -screen 0 1600x1000x24 >/tmp/xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2
/work/dsChirp/bin/dsChirp >/tmp/app.log 2>&1 &
APP_PID=$!
sleep 6

WID=$(xdotool search --name "." 2>/dev/null | tail -1)
if [ -n "${WID:-}" ]; then
  xdotool windowsize "$WID" 1600 1000 2>/dev/null || true
  xdotool windowmove "$WID" 0 0 2>/dev/null || true
  sleep 2
fi
import -window root "$OUTDIR/step0_main.png" 2>/dev/null

i=0
for spec in "$@"; do
  i=$((i+1))
  xy="${spec%%:*}"; label="${spec##*:}"
  [ "$label" = "$xy" ] && label="step$i"
  x="${xy%%,*}"; y="${xy##*,}"
  echo "### click $i at $x,$y ($label)"
  xdotool mousemove "$x" "$y" click 1
  sleep 4
  import -window root "$OUTDIR/step${i}_${label}.png" 2>/dev/null
done

echo "### chirp_config.py anywhere on disk:"
find /tmp /root /home -name 'chirp_config.py' 2>/dev/null | while read -r f; do
  echo "--- $f"; cp "$f" "$OUTDIR/oracle_chirp_config.py"; cat "$f"
done

echo "### settings as the app left them (this is the INPUT that produced the above):"
cp /root/.config/dsChirp/config.ini   "$OUTDIR/oracle_config.ini"   2>/dev/null
cp /root/.config/dsChirp/schedule.ini "$OUTDIR/oracle_schedule.ini" 2>/dev/null
cat /root/.config/dsChirp/config.ini

echo "### app log:"
for f in /tmp/igs/logs/*.log; do [ -e "$f" ] && { cp "$f" "$OUTDIR/oracle_app.log"; tail -30 "$f"; }; done

kill $APP_PID 2>/dev/null; kill $XVFB_PID 2>/dev/null; wait 2>/dev/null
exit 0

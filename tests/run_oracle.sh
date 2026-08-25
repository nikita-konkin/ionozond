#!/bin/bash
# Run the ORIGINAL dsChirp binary as a reference oracle and collect everything it
# emits, so the reconstruction can be diffed against it.
#
#   docker run --rm -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
#       dschirp-dev bash /work/ionozond/tests/run_oracle.sh [seconds]
set -u
SECS="${1:-10}"
FIX=/work/ionozond/tests/fixtures
OUT=/tmp/oracle

rm -rf "$OUT" /root/.config/dsChirp
mkdir -p /root/.config/dsChirp "$OUT" /tmp/igs/logs /tmp/out
cp "$FIX/config.ini" "$FIX/schedule.ini" /root/.config/dsChirp/

export QT_QPA_PLATFORM=offscreen
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR"

echo "### running original dsChirp for ${SECS}s"
timeout "$SECS" /work/dsChirp/bin/dsChirp > "$OUT/stdout.txt" 2>&1
echo "### exit: $?"

echo; echo "### stdout/stderr:"; cat "$OUT/stdout.txt"
echo; echo "### app log files:"; ls -la /tmp/igs/logs/ 2>&1
for f in /tmp/igs/logs/*.log; do
  [ -e "$f" ] || continue
  echo; echo "### $f:"; cat "$f"
done
echo; echo "### generated chirp_config.py:"
cat /tmp/out/chirp_config.py 2>&1 || echo "(not generated - session never started)"
echo; echo "### settings after run:"
cat /root/.config/dsChirp/config.ini

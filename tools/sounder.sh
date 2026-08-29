#!/bin/bash
#
# The console's sounding program.
#
# Set this as "Программа зондирования" in the parameters dialog. The console
# launches it with two arguments -- the generated config, and the archive
# directory -- and reads its output: STATUS lines drive the session panel,
# everything else lands in the log pane.
#
#   sounder.sh <chirp_config.py> <archive-dir>
#
# Anything in SOUNDER_ARGS is appended, so the radio address and tuning can be
# changed without editing this file:
#
#   SOUNDER_ARGS="--buffers 32 --args addr=192.168.10.3"
#
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-/tmp/out/chirp_config.py}"
ARCHIVE="${2:-}"

ARGS=(--config "$CONFIG" --loop)
[ -n "$ARCHIVE" ] && ARGS+=(--outdir "$ARCHIVE")

# Unbuffered, or the console sees nothing until a pipe buffer fills. The
# sounder flushes its own status lines, but this covers tracebacks too.
exec python3 -u "$HERE/rx_dechirp.py" "${ARGS[@]}" ${SOUNDER_ARGS:-}

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

# Jumbo frames, when the link to the radio actually carries them.
#
# UHD probes the path and falls back to 1472-byte payloads when the probe is
# inconclusive -- observed on this station with the interface MTU sitting at
# 9000. At 1472 bytes, 25 MS/s is about 68000 packets per second; at 8000 it is
# under 12500, and the packet rate is what the NIC's receive ring has to keep
# up with. So ask explicitly rather than accept the fallback.
#
# Skipped when SOUNDER_ARGS already carries --args, since argparse keeps only
# the last one and silently dropping the operator's address would be worse than
# not tuning. Set RADIO_ADDR to point the MTU lookup elsewhere, or
# NO_JUMBO=1 to leave UHD to its own devices.
if [ "${NO_JUMBO:-0}" != "1" ] && [[ "${SOUNDER_ARGS:-}" != *"--args"* ]]; then
    ADDR="${RADIO_ADDR:-192.168.10.2}"
    MTU=$(ip -o route get "$ADDR" 2>/dev/null           | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}'           | head -1           | xargs -I{} cat /sys/class/net/{}/mtu 2>/dev/null)
    if [ -n "${MTU:-}" ] && [ "$MTU" -ge 4000 ] 2>/dev/null; then
        FRAME=$(( MTU - 28 ))
        [ "$FRAME" -gt 8000 ] && FRAME=8000
        echo "link to $ADDR has MTU $MTU; asking UHD for ${FRAME}-byte frames"
        echo "  (NO_JUMBO=1 disables this; if the stream dies at once, use it)"
        ARGS+=(--args "recv_frame_size=$FRAME,send_frame_size=$FRAME")
    fi
fi

# Unbuffered, or the console sees nothing until a pipe buffer fills. The
# sounder flushes its own status lines, but this covers tracebacks too.
exec python3 -u "$HERE/rx_dechirp.py" "${ARGS[@]}" ${SOUNDER_ARGS:-}

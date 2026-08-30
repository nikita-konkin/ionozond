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
# OPT-IN, not opt-out. A host MTU of 9000 does not mean the radio and every
# hop between will carry 8000-byte frames: asked for them on this station, UHD
# agreed, and then the stream died on the first sample of every sounding until
# the run guard stopped it. UHD honours the request without checking that the
# other end can honour it, so the only proof is a capture that completes.
#
# Find the largest size this link actually carries with
# tools/host-setup/17-probe-frame-size.sh, then set JUMBO to it.
#
# Skipped when SOUNDER_ARGS already carries --args, since argparse keeps only
# the last one and silently dropping the operator's address would be worse than
# not tuning. RADIO_ADDR points the MTU lookup elsewhere.
if [ "${JUMBO:-0}" != "0" ] && [[ "${SOUNDER_ARGS:-}" != *"--args"* ]]; then
    ADDR="${RADIO_ADDR:-192.168.10.2}"
    MTU=$(ip -o route get "$ADDR" 2>/dev/null           | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}'           | head -1           | xargs -I{} cat /sys/class/net/{}/mtu 2>/dev/null)
    # JUMBO=1 means "work it out from the MTU"; JUMBO=<n> means "use n".
    if [ "${JUMBO}" = "1" ]; then
        FRAME=""
        if [ -n "${MTU:-}" ] && [ "$MTU" -ge 4000 ] 2>/dev/null; then
            FRAME=$(( MTU - 28 ))
            [ "$FRAME" -gt 8000 ] && FRAME=8000
        fi
    else
        FRAME="${JUMBO}"
    fi
    if [ -n "${FRAME:-}" ]; then
        echo "JUMBO set: asking UHD for ${FRAME}-byte frames (link MTU ${MTU:-?})"
        echo "  if soundings fail immediately, this link cannot carry them --"
        echo "  drop JUMBO, or probe with 17-probe-frame-size.sh"
        ARGS+=(--args "recv_frame_size=$FRAME,send_frame_size=$FRAME")
    fi
fi

# Unbuffered, or the console sees nothing until a pipe buffer fills. The
# sounder flushes its own status lines, but this covers tracebacks too.
exec python3 -u "$HERE/rx_dechirp.py" "${ARGS[@]}" ${SOUNDER_ARGS:-}

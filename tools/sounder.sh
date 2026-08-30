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

# One process can hold the radio. Started by hand while the service has it --
# from a terminal, or from the console's START button -- UHD fails to open the
# device and says so in its own terms, which look nothing like "something else
# is already using this". Say it plainly instead.
#
# IONOZOND_SERVICE is set by the unit, so this does not fire on itself.
if [ -z "${IONOZOND_SERVICE:-}" ]    && command -v systemctl >/dev/null 2>&1    && systemctl is-active --quiet ionozond-sounder 2>/dev/null; then
    echo "*** ionozond-sounder.service is already running and holds the radio." >&2
    echo "*** Only one sounder can have it. Either use the service:" >&2
    echo "***     journalctl -u ionozond-sounder -f" >&2
    echo "*** or stop it and run by hand:" >&2
    echo "***     sudo systemctl stop ionozond-sounder" >&2
    echo "*** The console can still display the archive either way; it is only" >&2
    echo "*** its START button that conflicts." >&2
    exit 3
fi
CONFIG="${1:-/tmp/out/chirp_config.py}"
ARCHIVE="${2:-}"

ARGS=(--config "$CONFIG" --loop)
[ -n "$ARCHIVE" ] && ARGS+=(--outdir "$ARCHIVE")

# Device arguments: the radio's address and, optionally, the frame size.
#
# Composed into ONE --args, because argparse keeps only the last occurrence --
# passing address and frame size as two flags silently discards the first.
# SOUNDER_ARGS carrying its own --args overrides all of this.
#
# Jumbo frames are OPT-IN. UHD probes the path and falls back to 1472-byte
# payloads when the probe is inconclusive, which it was on this station with
# the interface MTU sitting at 9000. At 1472 bytes 25 MS/s is about 68000
# packets per second; at 4000 it is under 26000, and packet rate is what the
# NIC's receive ring has to keep up with.
#
# But a host MTU of 9000 does not mean the radio and every hop between will
# carry 8000-byte frames. Asked for 8000 here, UHD agreed and then every
# sounding died on its first samples until the run guard stopped the loop; 4000
# carries fine. UHD honours the request without checking the far end can, so
# the only proof is a capture that completes. Find the largest size that works
# with tools/host-setup/17-probe-frame-size.py, then set JUMBO to it.
if [[ "${SOUNDER_ARGS:-}" != *"--args"* ]]; then
    DEVARGS=""
    [ -n "${RADIO_ADDR:-}" ] && DEVARGS="addr=${RADIO_ADDR}"

    if [ "${JUMBO:-0}" != "0" ]; then
        MTU=$(ip -o route get "${RADIO_ADDR:-192.168.10.2}" 2>/dev/null               | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}'               | head -1               | xargs -I{} cat /sys/class/net/{}/mtu 2>/dev/null)
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
            echo "JUMBO=${JUMBO}: asking UHD for ${FRAME}-byte frames (link MTU ${MTU:-?})"
            echo "  if soundings fail immediately this link cannot carry them;"
            echo "  unset JUMBO, or probe with 17-probe-frame-size.py"
            [ -n "$DEVARGS" ] && DEVARGS="${DEVARGS},"
            DEVARGS="${DEVARGS}recv_frame_size=${FRAME},send_frame_size=${FRAME}"
        fi
    fi

    [ -n "$DEVARGS" ] && ARGS+=(--args "$DEVARGS")
fi

# Unbuffered, or the console sees nothing until a pipe buffer fills. The
# sounder flushes its own status lines, but this covers tracebacks too.
exec python3 -u "$HERE/rx_dechirp.py" "${ARGS[@]}" ${SOUNDER_ARGS:-}

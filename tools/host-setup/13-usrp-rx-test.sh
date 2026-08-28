#!/bin/bash
#
# Prove the receive path actually carries the sounder's data rate.
#
#   sudo bash 13-usrp-rx-test.sh [usrp-ip] [seconds]
#   sudo bash 13-usrp-rx-test.sh 192.168.10.3 20
#
# uhd_usrp_probe only proves the radio answers a few control packets. The
# sounder needs 25 MS/s complex sustained for 250 seconds at a time, which is
# 100 MB/s and the N210's documented ceiling on gigabit Ethernet. This runs a
# rate ladder and counts what was lost.
#
# Read-only: it streams samples and throws them away. It changes no
# configuration and writes no files.
#
# Run 12-host-tuning.sh first, or the top of the ladder will fail for reasons
# that have nothing to do with the radio.
#
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IP="${1:-192.168.10.3}"
DURATION="${2:-10}"

# LFRX presents four frontends: A, B, and the two complex pairings AB and BA.
# A single HF antenna on RXA means frontend A - a real input, which the FPGA's
# digital downconverter turns into clean complex baseband. Letting UHD pick the
# default risks landing on AB, which pairs RXA as I with RXB as Q; with nothing
# on RXB that halves the amplitude and mirrors the spectrum.
SUBDEV="${SUBDEV:-A:A}"

# 20 MHz is the centre frequency the archive was recorded at.
FREQ="${FREQ:-20e6}"
RATES="${RATES:-5e6,12.5e6,25e6}"

if [ "$(id -u)" != "0" ]; then
    echo "run with sudo - UHD wants real-time priority, and root always has it"
    echo "(without it the top of the ladder reports losses that are the"
    echo "scheduler's fault, not the radio's)"
    exit 2
fi

find_uhd_tool() {
    local name="$1" p
    if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return 0; fi
    for p in /usr/lib/uhd/examples /usr/share/uhd/examples \
             /usr/lib/x86_64-linux-gnu/uhd/examples \
             /usr/local/lib/uhd/examples /usr/lib/uhd/utils; do
        [ -x "$p/$name" ] && { echo "$p/$name"; return 0; }
    done
    return 1
}

echo "=================================================================="
echo " USRP receive path test   $IP"
echo "=================================================================="

# ---- 0. is the host ready? ------------------------------------------------
echo
echo "--- 0. host readiness ------------------------------------------------"
RMEM=$(sysctl -n net.core.rmem_max 2>/dev/null)
echo "  net.core.rmem_max = ${RMEM:-?}"
if [ "${RMEM:-0}" -lt 50000000 ]; then
    echo "  *** too small. At 100 MB/s this buffer holds a couple of"
    echo "  *** milliseconds. Run 12-host-tuning.sh first or the 25 MS/s"
    echo "  *** step below will fail regardless of the radio."
    echo
fi
PREFIX=$(echo "$IP" | cut -d. -f1-3)
for I in $(ls /sys/class/net 2>/dev/null | grep -v lo); do
    if ip -4 addr show "$I" 2>/dev/null | grep -q "inet ${PREFIX}\."; then
        echo "  link to the radio: $I  MTU $(cat "/sys/class/net/$I/mtu" 2>/dev/null)"
    fi
done

# ---- 1. references and GPS ------------------------------------------------
echo
echo "--- 1. clock and time references -------------------------------------"
echo "  This radio has an internal Jackson-Labs FireFly GPSDO. It matters more"
echo "  here than in most applications: the dechirp replica has to track the"
echo "  transmitter's sweep, and absolute time decides where the delay axis"
echo "  starts. A free-running TCXO is parts in 10^6; a locked GPSDO is parts"
echo "  in 10^11."
echo
if QG=$(find_uhd_tool query_gpsdo_sensors); then
    echo "  using $QG"
    "$QG" --args "addr=$IP" 2>&1 | sed 's/^/    /'
elif [ -r "$SCRIPT_DIR/rx_rate_test.py" ] && python3 -c 'import uhd' >/dev/null 2>&1; then
    python3 "$SCRIPT_DIR/rx_rate_test.py" --args "addr=$IP" \
        --subdev "$SUBDEV" --sensors 2>&1 | grep -v '^\[INFO\]'
else
    echo "  no way to read the sensors here. Either install python3-uhd:"
    echo "      sudo apt-get install -y python3-uhd"
    echo "  or keep rx_rate_test.py beside this script."
    echo "  Skipping - informational, not a blocker."
fi

# ---- 2. the rate ladder ---------------------------------------------------
echo
echo "--- 2. receive rate ladder -------------------------------------------"
STATUS=2
if BR=$(find_uhd_tool benchmark_rate); then
    echo "  using $BR"
    echo "  subdev $SUBDEV, ${DURATION}s per step"
    echo
    WORST=0
    for RATE in $(echo "$RATES" | tr ',' ' '); do
        printf '  %-9s ' "$RATE"
        OUT=$("$BR" --args "addr=$IP" --rx_rate "$RATE" --duration "$DURATION" \
                    --rx_subdev "$SUBDEV" 2>&1)
        if echo "$OUT" | grep -qiE 'unrecognised option|unknown option'; then
            OUT=$("$BR" --args "addr=$IP" --rx_rate "$RATE" --duration "$DURATION" 2>&1)
        fi
        DROPPED=$(echo "$OUT" | awk -F: '/Num dropped samples/{gsub(/ /,"",$2); print $2}')
        OVER=$(echo "$OUT"    | awk -F: '/Num overruns detected/{gsub(/ /,"",$2); print $2}')
        SEQ=$(echo "$OUT"     | awk -F: '/Num sequence errors \(Rx\)/{gsub(/ /,"",$2); print $2}')
        RECV=$(echo "$OUT"    | awk -F: '/Num received samples/{gsub(/ /,"",$2); print $2}')
        if [ -z "${RECV:-}" ]; then
            echo "FAILED to run"
            echo "$OUT" | grep -iE 'error|exception|refus' | head -4 | sed 's/^/      /'
            WORST=2
            continue
        fi
        BAD=$(( ${DROPPED:-0} + ${OVER:-0} + ${SEQ:-0} ))
        if [ "$BAD" -eq 0 ]; then
            echo "clean   ($RECV samples, 0 dropped, 0 overruns)"
        else
            echo "LOSSES  ($RECV samples, ${DROPPED:-?} dropped, ${OVER:-?} overruns, ${SEQ:-?} seq errors)"
            [ "$WORST" -lt 1 ] && WORST=1
        fi
    done
    STATUS=$WORST
elif [ -r "$SCRIPT_DIR/rx_rate_test.py" ] && python3 -c 'import uhd' >/dev/null 2>&1; then
    # UHD's C++ benchmark_rate is not packaged for Ubuntu 24.04 - uhd-host
    # installs the utilities but not the examples. Measure it ourselves.
    echo "  benchmark_rate is not installed (Ubuntu's uhd-host omits the"
    echo "  examples), so measuring through python3-uhd instead."
    echo
    python3 "$SCRIPT_DIR/rx_rate_test.py" --args "addr=$IP" \
        --rates "$RATES" --duration "$DURATION" \
        --subdev "$SUBDEV" --freq "$FREQ" 2>&1 | grep -v '^\[INFO\]'
    STATUS=${PIPESTATUS[0]}
else
    echo "  nothing available to measure with. Either:"
    echo "      sudo apt-get install -y python3-uhd     # then rerun this"
    echo "  or find UHD's examples, if your packaging ships them:"
    echo "      dpkg -L uhd-host | grep -i example"
    exit 1
fi

# ---- 3. verdict -----------------------------------------------------------
echo
echo "--- 3. verdict --------------------------------------------------------"
case "$STATUS" in
0)
    echo "  The receive path carries 25 MS/s cleanly. That is what the sounder"
    echo "  needs and it is the N210's maximum over gigabit Ethernet, so there"
    echo "  is no headroom above this - keep the tuning in place."
    ;;
1)
    echo "  Samples were lost. In order of likelihood:"
    echo "    1. socket buffers still small     -> run 12-host-tuning.sh"
    echo "    2. MTU 1500                       -> SET_MTU=9000, if the NIC allows"
    echo "    3. CPU scaling down between bursts-> governor 'performance'"
    echo "    4. another process on the same NIC-> the USRP link must be alone"
    echo "  Losses only at the top of the ladder mean a throughput limit."
    echo "  Losses at every rate mean something structural - a duplex mismatch"
    echo "  or a shared interface."
    ;;
*)
    echo "  The measurement could not run. Check the errors above; if it says"
    echo "  the device is busy, another UHD process still holds it."
    ;;
esac

echo
echo "=================================================================="
echo " nothing was changed"
echo "=================================================================="
exit 0

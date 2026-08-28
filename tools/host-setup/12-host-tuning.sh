#!/bin/bash
#
# Make the host able to sustain the sounder's stream.
#
#   sudo bash 12-host-tuning.sh [interface]
#   sudo DRY_RUN=1 bash 12-host-tuning.sh eno1      # print, change nothing
#   sudo SET_MTU=9000 bash 12-host-tuning.sh eno1   # also raise the link MTU
#
# Why this is needed
# ------------------
# The sounder samples at 25 MS/s complex and dechirps on the host before
# decimating by 625 to the 40 kS/s that lands in the .lfs file. The dechirp has
# to see the whole swept band, so the full 25 MS/s crosses the Ethernet link:
#
#     25e6 samples/s * 4 bytes (sc16 over the wire) = 100 MB/s = 800 Mbit/s
#
# That is the N210's documented maximum over 1 GbE. At that rate the defaults
# do not hold: uhd_usrp_probe reported a receive socket stuck at 212992 bytes,
# which is about 2 ms of slack. One scheduler hiccup and samples are lost.
#
# Nothing here touches routing, the default route, or the WiFi profile, so it
# cannot affect a remote session. The only optional network change is the MTU
# of the USRP link itself, which is off unless you ask for it.
#
set -u

IFACE="${1:-}"
DRY_RUN="${DRY_RUN:-0}"
SET_MTU="${SET_MTU:-0}"
CON="usrp-link"

# Exactly the sizes UHD asked for in its warnings.
RMEM=50000000
WMEM=2500000

SYSCTL_FILE=/etc/sysctl.d/75-uhd-buffers.conf
LIMITS_FILE=/etc/security/limits.d/uhd-rtprio.conf
TARGET_USER="${SUDO_USER:-${USER:-root}}"

if [ "$(id -u)" != "0" ]; then
    echo "run with sudo"; exit 2
fi

say() { printf '%s\n' "$*"; }
run() {
    if [ "$DRY_RUN" = "1" ]; then
        say "    [dry-run] $*"
    else
        eval "$@"
    fi
}

echo "=================================================================="
echo " host tuning for a 25 MS/s USRP stream"
[ "$DRY_RUN" = "1" ] && echo " DRY RUN - nothing will be changed"
echo "=================================================================="

# ---- 1. socket buffers ----------------------------------------------------
echo
echo "--- 1. socket buffers ------------------------------------------------"
CUR_R=$(sysctl -n net.core.rmem_max 2>/dev/null)
CUR_W=$(sysctl -n net.core.wmem_max 2>/dev/null)
say "  now:    rmem_max=${CUR_R:-?}  wmem_max=${CUR_W:-?}"
say "  wanted: rmem_max=$RMEM  wmem_max=$WMEM"

if [ "${CUR_R:-0}" -ge "$RMEM" ] && [ "${CUR_W:-0}" -ge "$WMEM" ]; then
    say "  already large enough - nothing to do"
else
    say "  writing $SYSCTL_FILE and applying now"
    if [ "$DRY_RUN" = "1" ]; then
        say "    [dry-run] would write $SYSCTL_FILE with rmem_max/wmem_max"
    else
        {
            echo "# UHD needs a large receive socket for high-rate USRP streaming."
            echo "# The N210 sounder runs at 25 MS/s = 100 MB/s; the stock"
            echo "# 212992-byte buffer holds about 2 ms of that."
            echo "net.core.rmem_max = $RMEM"
            echo "net.core.wmem_max = $WMEM"
        } > "$SYSCTL_FILE"
    fi
    run "sysctl -q -w net.core.rmem_max=$RMEM"
    run "sysctl -q -w net.core.wmem_max=$WMEM"
    if [ "$DRY_RUN" != "1" ]; then
        say "  after:  rmem_max=$(sysctl -n net.core.rmem_max)  wmem_max=$(sysctl -n net.core.wmem_max)"
    fi
fi

# ---- 2. real-time priority ------------------------------------------------
echo
echo "--- 2. real-time thread priority -------------------------------------"
say "  uhd_usrp_probe reported: error in pthread_setschedparam"
say "  UHD wants to raise the receive thread's priority so the kernel cannot"
say "  preempt it mid-burst. An unprivileged process may only do that if it"
say "  belongs to a group carrying an rtprio limit."
echo

if ! getent group usrp >/dev/null 2>&1; then
    say "  group 'usrp' does not exist - creating it"
    run "groupadd -r usrp"
else
    say "  group 'usrp' exists"
fi

EXISTING_LIMIT=$(grep -rhsE '^[[:space:]]*@usrp[[:space:]]+.*rtprio' \
    /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null | head -1)
if [ -n "$EXISTING_LIMIT" ]; then
    say "  rtprio limit already present:"
    say "      $EXISTING_LIMIT"
else
    say "  writing $LIMITS_FILE"
    if [ "$DRY_RUN" = "1" ]; then
        say "    [dry-run] would write $LIMITS_FILE granting @usrp rtprio 99"
    else
        {
            echo "# Allow members of 'usrp' to run UHD's receive thread at"
            echo "# real-time priority."
            echo "@usrp   - rtprio  99"
            echo "@usrp   - memlock unlimited"
        } > "$LIMITS_FILE"
    fi
fi

if id -nG "$TARGET_USER" 2>/dev/null | tr ' ' '\n' | grep -qx usrp; then
    say "  $TARGET_USER is already in group usrp"
else
    say "  adding $TARGET_USER to group usrp"
    run "usermod -aG usrp '$TARGET_USER'"
    say
    say "  *** Group membership and rtprio limits only apply to a NEW login."
    say "  *** Log out and back in (or reboot) before running the sounder as"
    say "  *** $TARGET_USER. Running the test script under sudo works right"
    say "  *** away, because root is not subject to the limit."
fi

# ---- 3. MTU on the USRP link (opt-in) -------------------------------------
echo
echo "--- 3. link MTU ------------------------------------------------------"
if [ -z "$IFACE" ]; then
    say "  no interface given - skipping (pass one to check or set the MTU)"
else
    CUR_MTU=$(cat "/sys/class/net/$IFACE/mtu" 2>/dev/null)
    say "  $IFACE MTU is ${CUR_MTU:-?}"
    if [ "${CUR_MTU:-0}" -lt 4000 ]; then
        say "  At 1500 the radio sends ~1472-byte payloads: about 68000 packets"
        say "  per second at 25 MS/s. Jumbo frames cut that roughly fivefold and"
        say "  take a large bite out of CPU time spent handling interrupts."
    fi
    if [ "$SET_MTU" = "0" ]; then
        say
        say "  Not changing it. To raise it:"
        say "      sudo SET_MTU=9000 bash $0 $IFACE"
        say "  This only alters the USRP link, never the interface carrying your"
        say "  session - but not every NIC supports jumbo frames, so it is opt-in."
    elif ! nmcli -t -f NAME connection show 2>/dev/null | grep -qx "$CON"; then
        say "  profile '$CON' does not exist - run 10-usrp-link.sh first"
    else
        say "  setting $CON MTU to $SET_MTU and reactivating"
        run "nmcli connection modify '$CON' 802-3-ethernet.mtu '$SET_MTU'"
        run "nmcli connection up '$CON' >/dev/null"
        if [ "$DRY_RUN" != "1" ]; then
            NEW_MTU=$(cat "/sys/class/net/$IFACE/mtu" 2>/dev/null)
            say "  $IFACE MTU is now ${NEW_MTU:-?}"
            if [ "${NEW_MTU:-0}" != "$SET_MTU" ]; then
                say "  *** It did not take. The NIC or its driver probably caps"
                say "  *** the MTU below $SET_MTU. Try 4000, or leave it at 1500 -"
                say "  *** 25 MS/s is reachable without jumbo frames, just with"
                say "  *** more CPU spent on packets."
            fi
            say
            say "  The radio must agree too. Re-probe and check the frame size"
            say "  grew past 1472:"
            say "      uhd_usrp_probe --args=\"addr=USRP_IP\" 2>&1 | grep 'frame size'"
        fi
    fi
fi

# ---- 4. CPU ---------------------------------------------------------------
echo
echo "--- 4. CPU ------------------------------------------------------------"
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
say "  governor: ${GOV:-unknown}   cores: $(nproc)"
if [ "${GOV:-}" = "powersave" ]; then
    say "  'powersave' lets the clock drop between bursts, which shows up as"
    say "  overruns at high rates. Not changed here - it is a real tradeoff on"
    say "  a laptop. To try it for this boot only:"
    say "      sudo cpupower frequency-set -g performance"
fi

echo
echo "=================================================================="
if [ "$DRY_RUN" = "1" ]; then
    echo " dry run - nothing was changed"
else
    echo " done. Prove it with:  sudo bash 13-usrp-rx-test.sh 192.168.10.3"
fi
echo "=================================================================="

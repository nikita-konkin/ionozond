#!/bin/bash
#
# Point the host clock at an NTP server, and prove it took.
#
#   sudo bash 14-time-sync.sh                     # ns1.volgatech.net
#   sudo bash 14-time-sync.sh pool.ntp.org
#   sudo bash 14-time-sync.sh --from-gps          # no network; use the GPSDO
#
# Why this matters for a sounder
# ------------------------------
# The host clock decides which repetition window is next, and names every
# capture and log line. It does not decide when the sweep starts -- that comes
# from the radio's GPS-disciplined clock -- so a wrong host clock does not ruin
# a capture. It does drift, though, and once the error approaches half a
# repetition period the wrong window gets chosen.
#
# On the first host tested here NTP was not merely wrong, it was off:
#
#   System clock synchronized: no
#   NTP service: inactive
#
# and the clock had drifted 3.9 s from GPS.
#
# Touches no routing and no interface, so it cannot affect a remote session.
#
set -u

SERVER="${1:-ns1.volgatech.net}"
DROPIN=/etc/systemd/timesyncd.conf.d/ionozond.conf
USRP_ARGS="${USRP_ARGS:-addr=192.168.10.3}"
WAIT_SECONDS="${WAIT_SECONDS:-45}"

if [ "$(id -u)" != "0" ]; then
    echo "run with sudo"; exit 2
fi

say() { printf '%s\n' "$*"; }

echo "=================================================================="
echo " host time synchronisation"
echo "=================================================================="

echo
echo "--- before -----------------------------------------------------------"
timedatectl | sed 's/^/  /'

# ---- the GPS route, for a site where NTP is blocked ----------------------
if [ "$SERVER" = "--from-gps" ]; then
    echo
    echo "--- setting the clock from the radio's GPSDO -------------------------"
    if ! python3 -c 'import uhd' >/dev/null 2>&1; then
        say "  python3-uhd is not installed:  sudo apt-get install -y python3-uhd"
        exit 1
    fi
    GPS_EPOCH=$(python3 - "$USRP_ARGS" <<'PYEOF' 2>/dev/null
import sys, uhd
u = uhd.usrp.MultiUSRP(sys.argv[1])
try:
    u.set_clock_source("gpsdo")
    u.set_time_source("gpsdo")
except Exception:
    pass
locked = str(getattr(u.get_mboard_sensor("gps_locked", 0), "value", "")).lower()
if "true" not in locked:
    sys.exit(1)
print(int(float(getattr(u.get_mboard_sensor("gps_time", 0), "value"))))
PYEOF
)
    if [ -z "${GPS_EPOCH:-}" ]; then
        say "  could not read a locked GPS time from the radio."
        say "  Either it has no satellite lock or the GPSDO did not answer --"
        say "  both have happened on this unit. Nothing was changed."
        exit 1
    fi
    say "  radio reports $(date -u -d "@$GPS_EPOCH" '+%Y-%m-%d %H:%M:%S') UTC"
    say "  this host says $(date -u '+%Y-%m-%d %H:%M:%S') UTC"
    say "  difference     $(( GPS_EPOCH - $(date -u +%s) )) s"
    say
    say "  NTP must be off for a manual set to stick:"
    timedatectl set-ntp false
    date -u -s "@$GPS_EPOCH" >/dev/null
    say "  clock set. Now $(date -u '+%Y-%m-%d %H:%M:%S') UTC"
    say
    say "  *** This is a one-shot correction, not discipline. The clock will"
    say "  *** drift again. Re-run it from cron, or better, get NTP working."
    say
    say "  *** NTP IS NOW OFF. If it was working, you have just replaced a"
    say "  *** disciplined clock with a single reading from a GPSDO that has"
    say "  *** been returning stale values on this unit. Put it back with:"
    say "  ***     sudo timedatectl set-ntp true"
    exit 0
fi

# ---- can we reach the server at all? ------------------------------------
echo
echo "--- is $SERVER reachable? --------------------------------"
ADDR=$(getent hosts "$SERVER" 2>/dev/null | awk '{print $1; exit}')
if [ -z "${ADDR:-}" ]; then
    say "  *** the name does not resolve."
    say "  *** If it is an internal server, this host may need the corporate"
    say "  *** DNS, or a route to the network it lives on. Nothing changed."
    say
    say "  Try a public server instead, or the radio's GPS:"
    say "      sudo bash $0 pool.ntp.org"
    say "      sudo bash $0 --from-gps"
    exit 1
fi
say "  resolves to $ADDR"

if ping -c2 -W2 "$ADDR" >/dev/null 2>&1; then
    say "  answers ping"
else
    say "  does not answer ping -- not conclusive, many servers drop ICMP"
fi

# ---- configure ----------------------------------------------------------
echo
echo "--- configuring systemd-timesyncd -----------------------------------"
mkdir -p "$(dirname "$DROPIN")"
cat > "$DROPIN" <<EOF
# Written by ionozond tools/host-setup/14-time-sync.sh
# The sounder host's clock names every capture and picks the repetition
# window; it must not free-run.
[Time]
NTP=$SERVER
EOF
say "  wrote $DROPIN:"
sed 's/^/      /' "$DROPIN"

timedatectl set-ntp true
systemctl restart systemd-timesyncd 2>/dev/null || true

# ---- did it take? -------------------------------------------------------
echo
echo "--- waiting up to ${WAIT_SECONDS}s for synchronisation ---------------"
SYNCED=no
for _ in $(seq 1 "$WAIT_SECONDS"); do
    if [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "yes" ]; then
        SYNCED=yes
        break
    fi
    sleep 1
done

echo
if [ "$SYNCED" = "yes" ]; then
    say "  SYNCHRONISED"
    timedatectl timesync-status 2>/dev/null | sed 's/^/    /'
    echo
    timedatectl | sed 's/^/  /'
    echo
    say "  The clock may have stepped. That is safe mid-run: captures start on"
    say "  the radio's clock, not this one."
    exit 0
fi

say "  *** still not synchronised after ${WAIT_SECONDS}s."
say
say "  The configuration is in place, so it may yet succeed -- timesyncd backs"
say "  off and retries. Check again with:"
say "      timedatectl timesync-status"
say "      journalctl -u systemd-timesyncd -n 20"
say
say "  If it never syncs, UDP port 123 to that server is blocked, which is"
say "  common on a corporate network. Two ways out:"
say "      sudo bash $0 pool.ntp.org      # if the outside is reachable"
say "      sudo bash $0 --from-gps        # no network at all: use the GPSDO"
exit 1

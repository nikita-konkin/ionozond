#!/bin/bash
#
# Bring up the direct link to a USRP N210, with an armed rollback.
#
#   sudo bash 10-usrp-link.sh <interface> [host-ip/prefix]
#   sudo bash 10-usrp-link.sh enp3s0
#   sudo bash 10-usrp-link.sh enp3s0 192.168.20.1/24     # if .10 collides
#
# Why this is safe to run over a remote session:
#
#   * it only ADDS a NetworkManager profile; the WiFi profile carrying your
#     session is never read, never modified, never brought down
#   * the new profile sets ipv4.never-default, so it cannot take over the
#     default route no matter what the other end advertises
#   * no gateway and no DNS are configured on it - it is a point-to-point link
#   * a rollback timer is armed BEFORE the profile is brought up. If you lose
#     contact, the profile deletes itself and the machine returns to exactly
#     the state it is in now.
#
# If everything works, run the cancel command it prints. If you do nothing,
# the change reverts on its own.
#
set -u

IFACE="${1:-}"
ADDR="${2:-192.168.10.1/24}"
CON="usrp-link"
ROLLBACK_UNIT="usrp-link-rollback"
ROLLBACK_SECONDS="${ROLLBACK_SECONDS:-420}"     # 7 minutes

if [ -z "$IFACE" ]; then
    echo "usage: sudo bash $0 <interface> [host-ip/prefix]"
    echo
    echo "wired interfaces on this machine:"
    for i in $(ls /sys/class/net | grep -v lo); do
        [ -d "/sys/class/net/$i/wireless" ] && continue
        printf "  %-12s %s\n" "$i" "$(cat /sys/class/net/$i/operstate 2>/dev/null)"
    done
    exit 2
fi

if [ "$(id -u)" != "0" ]; then
    echo "run with sudo"; exit 2
fi

# ---- refuse to touch the interface carrying the default route -------------
DEFAULT_IFACE=$(ip route show default | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}' | head -1)
echo "default route is on: ${DEFAULT_IFACE:-<none>}"
if [ "$IFACE" = "$DEFAULT_IFACE" ]; then
    echo
    echo "REFUSING: $IFACE currently carries the default route - that is very"
    echo "probably your remote session. Pick the interface the USRP is on."
    exit 1
fi

# ---- refuse if the subnet is already routed somewhere else ----------------
SUBNET=$(echo "$ADDR" | cut -d/ -f1 | cut -d. -f1-3)

# Compare the /24 of each existing route as fields rather than by regex.
# Matching "192.168.10" as a pattern is a trap: the dots are wildcards unless
# escaped, and it also matches 192.168.100.0/24 by prefix. Splitting on "." and
# comparing whole strings avoids both.
collisions=$(ip route show \
    | grep -vE "dev +$IFACE\b" \
    | while read -r line; do
          net=$(printf '%s\n' "$line" | awk '{print $1}' | cut -d/ -f1 | cut -d. -f1-3)
          [ "$net" = "$SUBNET" ] && printf '%s\n' "$line"
      done)

if [ -n "$collisions" ]; then
    echo
    echo "REFUSING: ${SUBNET}.0/24 is already routed on another interface:"
    printf '%s\n' "$collisions" | sed 's/^/  /'
    echo "Choose a different subnet, e.g.:  sudo bash $0 $IFACE 192.168.20.1/24"
    exit 1
fi

echo "interface     $IFACE"
echo "host address  $ADDR   (no gateway, no DNS, never-default)"
echo "rollback in   ${ROLLBACK_SECONDS}s unless cancelled"
echo

# ---- arm the rollback FIRST ----------------------------------------------
systemctl stop "${ROLLBACK_UNIT}.timer" 2>/dev/null || true
systemd-run --quiet --unit="$ROLLBACK_UNIT" --on-active="$ROLLBACK_SECONDS" \
    /bin/bash -c "nmcli connection down '$CON' >/dev/null 2>&1; \
                  nmcli connection delete '$CON' >/dev/null 2>&1; \
                  logger -t usrp-link 'rolled back'" \
    || { echo "could not arm rollback - refusing to continue"; exit 1; }
echo "rollback armed as ${ROLLBACK_UNIT}.timer"

# ---- create and raise the profile ----------------------------------------
nmcli connection delete "$CON" >/dev/null 2>&1 || true
nmcli connection add type ethernet ifname "$IFACE" con-name "$CON" \
    ipv4.method manual \
    ipv4.addresses "$ADDR" \
    ipv4.never-default yes \
    ipv4.ignore-auto-dns yes \
    ipv4.route-metric 1000 \
    ipv6.method disabled \
    connection.autoconnect no >/dev/null

if ! nmcli connection up "$CON" >/dev/null 2>&1; then
    echo "failed to bring up $CON - rolling back now"
    nmcli connection delete "$CON" >/dev/null 2>&1
    systemctl stop "${ROLLBACK_UNIT}.timer" 2>/dev/null
    exit 1
fi

echo
echo "--- state after the change ------------------------------------------"
ip -br addr show "$IFACE"
echo
echo "default route (must be unchanged, on ${DEFAULT_IFACE:-<none>}):"
ip route show default
echo
echo "still reaching the internet?"
ip route get 1.1.1.1 2>/dev/null | head -1

echo
echo "--- USRP ------------------------------------------------------------"
USRP_IP="${SUBNET}.2"
echo "pinging $USRP_IP ..."
if ping -c 3 -W 2 "$USRP_IP" >/dev/null 2>&1; then
    echo "  reachable"
    if command -v uhd_find_devices >/dev/null 2>&1; then
        echo
        uhd_find_devices --args="addr=$USRP_IP" 2>&1 | sed 's/^/  /'
    else
        echo "  (uhd_find_devices not installed - see the README)"
    fi
else
    echo "  no reply. Check the cable, and that the N210 is powered and has"
    echo "  its factory address ${SUBNET}.2. This does not affect your session."
fi

cat <<EOF

======================================================================
 If you can still read this, the change is good. Keep it:

     sudo systemctl stop ${ROLLBACK_UNIT}.timer
     sudo nmcli connection modify $CON connection.autoconnect yes

 Do nothing and it reverts by itself in ${ROLLBACK_SECONDS}s.

 To revert immediately:

     sudo nmcli connection delete $CON
======================================================================
EOF

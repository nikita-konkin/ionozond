#!/bin/bash
#
# Work out why a USRP is not answering. Read-only: sniffs and probes, never
# reconfigures anything.
#
#   sudo bash 11-usrp-diagnose.sh eno1 [usrp-ip]
#
# The important question it answers is what is actually on the far end of the
# cable. A quiet link is consistent with a point-to-point USRP; a link carrying
# ARP, DHCP and mDNS chatter is a LAN switch, which means the cable is in the
# wrong socket.
#
set -u

IFACE="${1:-}"
USRP_IP="${2:-192.168.10.2}"
SNIFF_SECONDS="${SNIFF_SECONDS:-20}"

if [ -z "$IFACE" ]; then
    echo "usage: sudo bash $0 <interface> [usrp-ip]"; exit 2
fi
if [ "$(id -u)" != "0" ]; then
    echo "run with sudo (tcpdump and arping need it)"; exit 2
fi

echo "=================================================================="
echo " USRP diagnosis on $IFACE   (read-only)"
echo "=================================================================="

echo
echo "--- 1. physical link ------------------------------------------------"
echo "  carrier   $(cat /sys/class/net/$IFACE/carrier 2>/dev/null)   (1 = cable connected and negotiated)"
echo "  speed     $(cat /sys/class/net/$IFACE/speed 2>/dev/null) Mb/s"
echo "  duplex    $(cat /sys/class/net/$IFACE/duplex 2>/dev/null)"
echo "  mtu       $(cat /sys/class/net/$IFACE/mtu 2>/dev/null)"
echo "  address:"
ip -br addr show "$IFACE" | sed 's/^/    /'
if command -v ethtool >/dev/null 2>&1; then
    echo "  link detected: $(ethtool "$IFACE" 2>/dev/null | awk -F': ' '/Link detected/{print $2}')"
fi

if [ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" != "1" ]; then
    echo
    echo "  NO CARRIER -- nothing is plugged in, or the far end is powered off."
    echo "  Nothing else here will work until that is fixed."
    exit 1
fi

echo
echo "--- 2. what is on the far end? --------------------------------------"
echo "  listening for ${SNIFF_SECONDS}s ..."
if command -v tcpdump >/dev/null 2>&1; then
    CAP=$(mktemp)
    timeout "$SNIFF_SECONDS" tcpdump -i "$IFACE" -n -e -l 2>/dev/null | head -40 > "$CAP"
    COUNT=$(wc -l < "$CAP")
    echo "  captured $COUNT frames"
    if [ "$COUNT" -gt 0 ]; then
        sed 's/^/    /' "$CAP" | head -15
    fi
    echo
    if [ "$COUNT" -eq 0 ]; then
        echo "  SILENT link. Consistent with a point-to-point cable to a USRP"
        echo "  that is powered but idle -- an N210 does not chatter unprompted."
    elif grep -qiE 'dhcp|bootp|mdns|ssdp|spanning tree|stp|cdp|lldp' "$CAP"; then
        echo "  *** This looks like a LAN, not a USRP ***"
        echo "  DHCP / mDNS / STP / discovery traffic means a switch is on the"
        echo "  far end. The cable is very probably in the wrong socket."
    else
        echo "  Some traffic, but no obvious LAN signature. See the frames above."
    fi
    rm -f "$CAP"
else
    echo "  tcpdump not installed -- skipping. Install with:"
    echo "      sudo apt-get install -y tcpdump"
fi

echo
echo "--- 3. does anything answer at layer 2? -----------------------------"
if command -v arping >/dev/null 2>&1; then
    arping -I "$IFACE" -c 4 -w 5 "$USRP_IP" 2>&1 | sed 's/^/  /'
else
    echo "  arping not installed:  sudo apt-get install -y iputils-arping"
fi
echo
echo "  neighbour table for this interface:"
ip neigh show dev "$IFACE" | sed 's/^/    /' || echo "    (empty)"

echo
echo "--- 4. ping ---------------------------------------------------------"
ping -c 3 -W 2 -I "$IFACE" "$USRP_IP" 2>&1 | tail -4 | sed 's/^/  /'

echo
echo "--- 5. UHD broadcast discovery --------------------------------------"
if command -v uhd_find_devices >/dev/null 2>&1; then
    echo "  this finds an N210 even if its IP is not what we expect,"
    echo "  because discovery is a broadcast on the link:"
    uhd_find_devices 2>&1 | sed 's/^/    /'
else
    echo "  uhd_find_devices NOT INSTALLED -- this is the single most useful"
    echo "  test available and it needs UHD:"
    echo
    echo "      sudo apt-get install -y uhd-host"
    echo "      sudo uhd_images_downloader"
    echo "      uhd_find_devices"
    echo
    echo "  Discovery is a UDP broadcast to port 49152, so it locates the radio"
    echo "  whatever address it holds, as long as it is on this cable."
fi

echo
echo "=================================================================="
echo " nothing was changed"
echo "=================================================================="

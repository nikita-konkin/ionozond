#!/usr/bin/env bash
# Why UHD prints "D": packets lost before the application ever saw them.
#
# "D" on stderr is a sequence gap -- the kernel or the NIC dropped datagrams.
# It is NOT the dechirp being slow; that shows as stall time and "O". At
# 25 MS/s the link carries 100 MB/s, four fifths of a gigabit, so the margin
# for a missed interrupt is small.
#
#   bash 16-check-packet-loss.sh [interface]
set -u
IFACE="${1:-}"
if [ -z "$IFACE" ]; then
    IFACE=$(ip -o route get 192.168.10.2 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}')
    IFACE="${IFACE:-$(ls /sys/class/net | grep -v lo | head -1)}"
fi
echo "interface: $IFACE"
echo

echo "== frame size =="
MTU=$(cat "/sys/class/net/$IFACE/mtu" 2>/dev/null || echo "?")
echo "  MTU $MTU"
if [ "$MTU" -lt 9000 ] 2>/dev/null; then
    echo "  *** UHD will use $((MTU - 28))-byte frames. At 100 MB/s that is"
    echo "  *** $((100000000 / (MTU - 28))) packets per second."
    echo "  *** Jumbo frames cut that by more than five, and with it the"
    echo "  *** chance of a drop. If the log says 'recv frame size: 1472'"
    echo "  *** the MTU never took. Set it on the CONNECTION, not the link,"
    echo "  *** or NetworkManager reverts it:"
    echo "  ***   sudo SET_MTU=9000 bash 12-host-tuning.sh $IFACE"
    echo "  *** The switch and the N210 port must both carry 9000 too."
else
    echo "  jumbo frames in use ($((MTU - 28))-byte payloads)"
fi

echo
echo "== NIC ring buffers =="
if command -v ethtool >/dev/null; then
    ethtool -g "$IFACE" 2>/dev/null | sed 's/^/  /' || echo "  not reported"
    echo "  raise with:  sudo ethtool -G $IFACE rx 4096"
else
    echo "  ethtool not installed:  sudo apt-get install -y ethtool"
fi

echo
echo "== drop counters =="
for f in rx_dropped rx_missed_errors rx_over_errors rx_fifo_errors; do
    v=$(cat "/sys/class/net/$IFACE/statistics/$f" 2>/dev/null || echo "-")
    echo "  $f: $v"
done
echo "  UDP receive errors (whole host):"
netstat -su 2>/dev/null | grep -iE 'receive (buffer )?errors|packet receive' | sed 's/^/    /' \
    || echo "    netstat not installed"

echo
echo "== socket buffer ceiling =="
echo "  net.core.rmem_max = $(sysctl -n net.core.rmem_max 2>/dev/null)"
echo "  the sounder asks UHD for recv_buff_size=100000000; the kernel silently"
echo "  clamps that to rmem_max, so rmem_max must be at least as large."

echo
echo "Re-run during a capture and watch whether the drop counters move."
echo "If they climb while rx_dechirp reports overflows, the loss is on the"
echo "wire or in the kernel, and no amount of CPU headroom will fix it."

#!/bin/bash
#
# Read-only inventory of a sounder host. Changes nothing, touches no
# interface, and is safe to run over a remote session.
#
# Run it and send the whole output back before any network change is made.
#
#   bash 00-inventory.sh
#
echo "=================================================================="
echo " host inventory   $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "=================================================================="

echo
echo "--- system ---------------------------------------------------------"
lsb_release -d 2>/dev/null || cat /etc/os-release | head -2
echo "kernel   $(uname -r)"
echo "cpu      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "cores    $(nproc)"
# /proc/meminfo rather than `free`, whose labels are localised
echo "memory   $(awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}END{printf "%.1f GiB total, %.1f GiB available", t/1048576, a/1048576}' /proc/meminfo)"

echo
echo "--- disk (captures are ~80 MB each, ~23 GB/day/station) -------------"
df -h --output=source,size,used,avail,pcent,target / /home /var 2>/dev/null | awk '!seen[$0]++'

echo
echo "--- interfaces -----------------------------------------------------"
ip -br link
echo
ip -br addr

echo
echo "--- link UP but no address (cable in, nothing configured) -----------"
found=0
for i in $(ls /sys/class/net | grep -v lo); do
    [ "$(cat /sys/class/net/$i/carrier 2>/dev/null)" = "1" ] || continue
    if ! ip -4 addr show "$i" 2>/dev/null | grep -q 'inet '; then
        drv=$(basename "$(readlink -f /sys/class/net/$i/device/driver 2>/dev/null)" 2>/dev/null)
        spd=$(cat /sys/class/net/$i/speed 2>/dev/null)
        echo "  $i  driver=${drv:-?}  speed=${spd:-?}   <- candidate USRP link"
        found=1
    fi
done
[ "$found" = "0" ] && echo "  none"

echo
echo "--- routing table --------------------------------------------------"
ip route show
echo
echo "default routes, by preference (lowest metric wins):"
ip route show default | sed 's/^/  /'

echo
echo "*** DEFAULT ROUTE - this is what carries your remote session ***"
ip route show default
echo
echo "path to the internet:"
ip route get 1.1.1.1 2>/dev/null

echo
echo "--- NetworkManager connections -------------------------------------"
nmcli -t -f NAME,UUID,TYPE,DEVICE,STATE connection show 2>/dev/null | column -t -s: 2>/dev/null \
    || nmcli connection show 2>/dev/null
echo
echo "autoconnect / metric / never-default per profile:"
# NOTE: `for c in $(nmcli ...)` word-splits on spaces, which mangles profile
# names like "Проводное подключение 1". Read line by line instead.
nmcli -t -f NAME connection show 2>/dev/null | while IFS= read -r c; do
    [ -n "$c" ] || continue
    printf "  %-26s " "$c"
    nmcli -t -f connection.autoconnect,ipv4.route-metric,ipv4.never-default,ipv4.method \
        connection show "$c" 2>/dev/null | tr '\n' ' '
    echo
done

echo
echo "--- does the USRP subnet collide with anything? --------------------"
if ip route show | grep -qE '192\.168\.10\.'; then
    echo "  *** COLLISION: 192.168.10.0/24 is already routed ***"
    ip route show | grep -E '192\.168\.10\.'
    echo "  -> the USRP will need a different subnet"
else
    echo "  no route covers 192.168.10.0/24 - the USRP default is usable"
fi

echo
echo "--- USB ethernet adapter -------------------------------------------"
lsusb 2>/dev/null | grep -iE 'ethernet|realtek|asix|ax88|rtl8|lan' || echo "  (nothing obvious in lsusb)"
for i in $(ls /sys/class/net | grep -v lo); do
    drv=$(basename "$(readlink -f /sys/class/net/$i/device/driver 2>/dev/null)" 2>/dev/null)
    spd=$(cat /sys/class/net/$i/speed 2>/dev/null)
    mtu=$(cat /sys/class/net/$i/mtu 2>/dev/null)
    printf "  %-12s driver=%-12s speed=%-8s mtu=%s\n" "$i" "${drv:-?}" "${spd:-?}" "${mtu:-?}"
done

echo
echo "--- UHD ------------------------------------------------------------"
if command -v uhd_find_devices >/dev/null 2>&1; then
    uhd_find_devices --version 2>&1 | head -3
    echo "images dir: $(uhd_config_info --images-dir 2>/dev/null || echo '?')"
else
    echo "  uhd_find_devices NOT FOUND - the uhd-host package is not installed"
fi
dpkg -l 2>/dev/null | awk '/uhd|gnuradio|digital.rf/{print "  "$2" "$3}' || true

echo
echo "--- remote session ---------------------------------------------------"
pgrep -a anydesk >/dev/null 2>&1 && echo "  anydesk is running" || echo "  anydesk not seen in process list"
echo "  established connections (needs sudo for process names):"
ss -tn state established 2>/dev/null | head -8

echo
echo "--- proxy currently configured? ------------------------------------"
env | grep -iE '^(http|https|no)_proxy' || echo "  no proxy in this shell's environment"
[ -f /etc/environment ] && grep -iE 'proxy' /etc/environment || echo "  none in /etc/environment"
[ -f /etc/apt/apt.conf.d/proxy.conf ] && echo "  apt proxy file exists" || echo "  no apt proxy file"

echo
echo "=================================================================="
echo " nothing was changed"
echo "=================================================================="

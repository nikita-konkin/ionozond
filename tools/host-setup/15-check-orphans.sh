#!/usr/bin/env bash
# Are there sounders running that no console owns?
#
# The console used to be destroyed without stopping its QProcess, which left
# python holding the radio and still writing captures. Two of those competing
# for one N210 is an overflow problem that appears from nowhere -- and an
# overflow shifts every later echo by the duration lost, so the ionogram is
# cut dead at whatever frequency the sweep had reached.
set -u
echo "== sounder processes =="
pgrep -af 'rx_dechirp\.py|sounder\.sh' || echo "  none"
echo
echo "== who holds the radio =="
if command -v ss >/dev/null; then
    ss -unap 2>/dev/null | grep -E ':4[0-9]{4}' | head || echo "  no UDP sockets on the USRP range"
fi
echo
echo "== real-time priority =="
if id -nG | tr ' ' '\n' | grep -qx usrp; then
    echo "  this session IS in group 'usrp'"
else
    echo "  *** this session is NOT in group 'usrp' -- UHD cannot raise its"
    echo "  *** receive thread priority, which is the usual cause of"
    echo "  *** sporadic overflows. Log out and back in, or run: newgrp usrp"
fi
grep -rhsE '^[[:space:]]*@usrp[[:space:]]+.*rtprio' \
    /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null \
    || echo "  *** no rtprio limit for @usrp; run 12-host-tuning.sh"
echo
echo "To stop every orphan:  pkill -f rx_dechirp.py"

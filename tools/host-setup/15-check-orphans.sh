#!/usr/bin/env bash
# Sounders nobody owns, and whether UHD can run its receive thread at speed.
#
# The console used to be destroyed without stopping its QProcess, which left
# python holding the radio and still writing captures. Two of those competing
# for one N210 is an overflow problem that appears from nowhere -- and an
# overflow shifts every later echo by the duration lost, so the ionogram is
# cut dead at whatever frequency the sweep had reached.
set -u

# $USER is not exported by every shell (cron, some su invocations).
USER="${USER:-$(id -un)}"

echo "== sounder processes =="
pgrep -af 'rx_dechirp\.py|sounder\.sh' || echo "  none"

echo
echo "== who holds the radio =="
if command -v ss >/dev/null; then
    ss -unap 2>/dev/null | grep -E ':4[0-9]{4}' | head || echo "  no UDP sockets on the USRP range"
fi

echo
echo "== real-time priority =="
# Group membership is necessary but not sufficient. What UHD needs is
# RLIMIT_RTPRIO above zero, and pam_limits grants that when the SESSION starts.
# Being added to a group afterwards changes nothing until a new login, and
# newgrp does not re-run pam_limits on most distributions.
RT=$(ulimit -Hr 2>/dev/null || echo 0)
echo "  rtprio hard limit for this session: ${RT}"

if getent group usrp 2>/dev/null | grep -q "[:,]${USER}\b"; then
    echo "  user '${USER}' IS a member of group 'usrp'"
else
    echo "  *** user '${USER}' is NOT in group 'usrp'"
    echo "  ***   sudo usermod -aG usrp ${USER}"
fi

if id -nG | tr ' ' '\n' | grep -qx usrp; then
    echo "  this session carries the group"
else
    echo "  *** this session predates the group being added"
fi

grep -rhsE '^[[:space:]]*@usrp[[:space:]]+.*rtprio' \
    /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null \
    || echo "  *** no rtprio limit for @usrp; run 12-host-tuning.sh"

if [ "${RT}" = "0" ]; then
    echo
    echo "  *** rtprio is 0, so UHD cannot raise its receive thread priority"
    echo "  *** regardless of group membership. pam_limits applies it at login."
    echo "  ***"
    echo "  *** DO NOT log out if a remote desktop is your only way in --"
    echo "  *** ending the session can take the remote desktop with it."
    echo "  *** Start a fresh PAM session instead and launch the console from"
    echo "  *** inside it, which applies the limit without logging out:"
    echo "  ***"
    echo "  ***   su - ${USER} -c 'DISPLAY=${DISPLAY:-:0} \$HOME/.cache/ionozond-build/ionozond'"
    echo "  ***"
    echo "  *** Confirm it took, inside that session:  ulimit -Hr   (want 99)"
fi

echo
echo "To stop every orphan:  pkill -f rx_dechirp.py"

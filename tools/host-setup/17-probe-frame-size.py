#!/usr/bin/env python3
"""Find the largest UDP frame size this link to the radio actually carries.

    python3 17-probe-frame-size.py [--addr 192.168.10.2] [--rate 25e6]

A host MTU of 9000 does not mean the radio will carry 8000-byte frames. UHD
honours a recv_frame_size request without checking the far end can honour it:
it reports the size it agreed to, and then the stream dies on the first sample
of every capture. Asked for 8000 on this station and that is exactly what
happened -- three soundings produced 972, 973 and 1944 samples out of 6.125
billion before the run guard stopped it.

So the only proof is samples arriving. This streams a short burst at each
candidate size and reports which ones deliver. Whatever the largest passing
size is, give it to the sounder as JUMBO=<size>.

Larger frames matter because the NIC's receive ring holds a number of *packets*
rather than a number of bytes: at 1472 bytes, 100 MB/s is about 68000 packets
per second and a 4096-entry ring covers 60 ms; at 8000 bytes it is under 12500
and the same ring covers 327 ms.
"""

import argparse
import sys
import time

try:
    import numpy as np
    import uhd
except ImportError as exc:
    sys.stderr.write("needs numpy and python3-uhd: %s\n" % exc)
    sys.exit(2)

# 1472 is the no-jumbo baseline: 1500 less IP and UDP headers. The rest step up
# to the 8000 the N210 is usually quoted as supporting.
CANDIDATES = [1472, 2000, 3000, 4000, 6000, 8000]

BURST_SECONDS = 0.25


def probe(addr, rate, frame):
    """Stream a short burst at one frame size. Returns (samples, note)."""
    args = "recv_frame_size=%d,send_frame_size=%d" % (frame, frame)
    if addr:
        args = "addr=%s,%s" % (addr, args)
    try:
        usrp = uhd.usrp.MultiUSRP(args)
    except Exception as exc:
        return 0, "could not open: %s" % exc

    try:
        usrp.set_rx_rate(rate)
        actual = usrp.get_rx_rate()
        st = uhd.usrp.StreamArgs("fc32", "sc16")
        streamer = usrp.get_rx_stream(st)
        spb = streamer.get_max_num_samps()
        buf = np.empty((1, spb), dtype=np.complex64)
        md = uhd.types.RXMetadata()

        cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
        cmd.stream_now = True
        streamer.issue_stream_cmd(cmd)

        got = 0
        errors = []
        deadline = time.time() + BURST_SECONDS + 1.0
        want = int(actual * BURST_SECONDS)
        while got < want and time.time() < deadline:
            n = streamer.recv(buf, md, 0.5)
            code = str(md.error_code).lower()
            if "none" not in code:
                errors.append(str(md.error_code))
                if n == 0:
                    break
            got += n

        streamer.issue_stream_cmd(
            uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
        # Drain, or the next probe meets stale packets out of sequence.
        drain_until = time.time() + 1.0
        while time.time() < drain_until:
            try:
                if streamer.recv(buf, md, 0.1) == 0:
                    break
            except Exception:
                break

        note = ""
        if errors:
            seen = sorted(set(errors))
            note = "%d error(s): %s" % (len(errors), ", ".join(seen[:2]))
        return got, note
    except Exception as exc:
        return 0, "failed: %s" % exc
    finally:
        del usrp


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", default="192.168.10.2")
    ap.add_argument("--rate", type=float, default=25e6)
    opts = ap.parse_args()

    want = int(opts.rate * BURST_SECONDS)
    print("=" * 62)
    print(" frame size probe: %.1f MS/s, %.2f s per size" % (opts.rate / 1e6, BURST_SECONDS))
    print(" a size passes only if samples actually arrive, not because")
    print(" UHD agreed to it -- agreeing is what it does regardless")
    print("=" * 62)
    print()
    print("  %-8s %-14s %-8s %s" % ("frame", "samples", "verdict", "notes"))

    best = None
    for frame in CANDIDATES:
        got, note = probe(opts.addr, opts.rate, frame)
        ok = got >= 0.5 * want
        if ok:
            best = frame
        print("  %-8d %-14s %-8s %s"
              % (frame, "%d / %d" % (got, want), "ok" if ok else "FAIL", note))

    print()
    if best is None:
        print("  Nothing passed, including the 1472 baseline. That is not a")
        print("  frame size problem -- check the link and the radio itself with")
        print("  11-usrp-diagnose.sh before going further.")
        return 1
    if best == CANDIDATES[0]:
        print("  Only the %d baseline passed. Leave JUMBO unset; the ring size" % best)
        print("  and rmem_max are the levers that remain.")
        return 0
    print("  Largest size that delivered samples: %d" % best)
    print()
    print("  Start the sounder with it:")
    print("      JUMBO=%d bash tools/sounder.sh <config> <archive>" % best)
    print()
    print("  Then confirm a whole 250 s sounding completes before trusting it.")
    print("  A quarter-second burst is necessary, not sufficient.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

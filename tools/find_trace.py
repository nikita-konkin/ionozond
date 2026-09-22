#!/usr/bin/env python3
"""Where is the echo, if it is anywhere?

    python3 tools/find_trace.py ~/ionograms/2026.09.22/lfm_ionogram-*.h5

For a new station the console's panel is the worst place to look: it shows a
+-1500 km window centred on the great-circle distance, so a trace that is
there but misplaced -- wrong chirptime, wrong coordinates, wrong sweep rate --
is simply not drawn, and the panel looks the same as one with no transmitter
at all. Those are very different faults and the fix for one does nothing for
the other.

The .h5 archive keeps 0-8000 km, so this profiles the whole of it and says
which ranges hold energy. Reading several files at once is the point: noise
moves between captures and an echo does not, so a peak that appears at the
same range in most of them is real and one that wanders is not.

What the answers mean
---------------------
  a peak near the expected range      the station works; the display window
                                      or the gate is what needs changing
  a peak somewhere else, consistent   timing or geometry: see the offset it
                                      prints in seconds of chirptime
  no peak above the noise anywhere    nothing was received. Not a settings
                                      problem: the transmitter is off, out of
                                      propagation, or too weak.
"""

import argparse
import glob
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))

import numpy as np


def earth_distance_km(lat1, lon1, lat2, lon2):
    """Great circle, the same 6371 km sphere lfp_products uses."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = (math.sin(dp / 2) ** 2 +
         math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2)
    return 2 * 6371.0 * math.asin(math.sqrt(a))


def profile(path):
    """Mean power against range for one archive, plus its axes."""
    import h5_archive
    a = h5_archive.read(path)
    power = h5_archive.power_from_snr(a["snr"])       # (n_freq, n_range)
    # Mean over frequency. A trace occupies a minority of the columns, so the
    # mean is a weak statistic -- but it is the one that does not reward a
    # single hot interference line the way a max would.
    return power.mean(axis=0), a


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archives", nargs="+", help=".h5 files, or a glob")
    ap.add_argument("--top", type=int, default=6,
                    help="how many range peaks to print (default 6)")
    ap.add_argument("--tx-lat", type=float, help="transmitter latitude")
    ap.add_argument("--tx-lon", type=float, help="transmitter longitude")
    ap.add_argument("--rx-lat", type=float, help="receiver latitude")
    ap.add_argument("--rx-lon", type=float, help="receiver longitude")
    opts = ap.parse_args()

    paths = []
    for pattern in opts.archives:
        paths += sorted(glob.glob(os.path.expanduser(pattern))) or [pattern]
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        sys.stderr.write("no archives found\n")
        return 2

    stack = []
    meta = None
    for p in paths:
        try:
            prof, a = profile(p)
        except Exception as exc:
            print("  %s: %s" % (os.path.basename(p), exc))
            continue
        stack.append(prof)
        meta = a
    if not stack:
        return 2

    n = min(len(s) for s in stack)
    stack = np.vstack([s[:n] for s in stack])
    ranges_km = meta["ranges_m"][:n] / 1e3

    # Median across captures, not mean: an echo repeats and interference does
    # not, so the median is what separates them.
    med = np.median(stack, axis=0)
    floor = np.median(med)
    excess_db = 10.0 * np.log10(np.maximum(med / max(floor, 1e-30), 1e-30))

    print("=" * 66)
    print(" %d archive(s), %s -> %s"
          % (len(stack), meta["tx_name"], meta["rx_name"]))
    print("=" * 66)
    print("  range axis   %.0f to %.0f km, %d gates of %.1f km"
          % (ranges_km[0], ranges_km[-1], n, ranges_km[1] - ranges_km[0]))
    print("  freq axis    %.2f to %.2f MHz"
          % (meta["freqs_hz"][0] / 1e6, meta["freqs_hz"][-1] / 1e6))
    print("  sweep rate   %.0f kHz/s" % (meta["rate"] / 1e3))

    expected = None
    if None not in (opts.tx_lat, opts.tx_lon, opts.rx_lat, opts.rx_lon):
        ground = earth_distance_km(opts.tx_lat, opts.tx_lon,
                                   opts.rx_lat, opts.rx_lon)
        expected = math.hypot(ground, 2 * 300.0)     # one F-layer hop
        print("  expected     %.0f km ground, ~%.0f km for a 1F hop (%.1f ms)"
              % (ground, expected, expected / 300.0))
    print()

    # Local maxima, so one broad peak is reported once rather than as its
    # every sample.
    order = np.argsort(excess_db)[::-1]
    picked = []
    for i in order:
        if any(abs(ranges_km[i] - ranges_km[j]) < 200.0 for j in picked):
            continue
        picked.append(i)
        if len(picked) >= opts.top:
            break

    print("  strongest ranges, above the median of the profile:")
    for i in sorted(picked, key=lambda k: -excess_db[k]):
        note = ""
        if expected is not None:
            off_km = ranges_km[i] - expected
            if abs(off_km) < 400.0:
                note = "  <- the expected 1F echo"
            else:
                # Timing shows up as range: c * dt. Reported in chirptime
                # seconds because that is the field the operator can change.
                dt = off_km / 300.0 / 1000.0
                note = "  (%+.0f km = %+.3f s of chirptime)" % (off_km, dt)
        print("    %8.0f km  %6.1f ms  %+5.1f dB%s"
              % (ranges_km[i], ranges_km[i] / 300.0, excess_db[i], note))

    best = excess_db[picked[0]]
    print()
    if best < 1.0:
        print("  Nothing stands above the noise anywhere in 0-8000 km.")
        print("  This is not a settings fault: with the wrong chirptime or the")
        print("  wrong coordinates a trace still appears, just in the wrong")
        print("  place. A flat profile means nothing was received -- the")
        print("  transmitter is off, out of propagation, or too weak here.")
    elif best < 3.0:
        print("  Marginal: %.1f dB is not convincing on its own. Re-run over a"
              % best)
        print("  day's archives before concluding anything.")
    else:
        print("  %.1f dB at %.0f km is a real echo."
              % (best, ranges_km[picked[0]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

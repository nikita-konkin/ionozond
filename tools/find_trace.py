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


def fold_seconds(rate, sr, limit=12):
    """Whole-second chirptime errors that land the echo exactly on the fold.

    A chirptime wrong by dt leaves the replica offset from the transmitter by
    a constant rate*dt -- constant, not a chirp, because both ramps have the
    same slope. That beat is sampled at `sr` and aliases, and when rate*dt is
    an odd multiple of sr/2 it aliases onto the Nyquist fold: the one place on
    the range axis where the positive and negative ends meet, so the echo
    appears at both of them and at neither true range.

    Returns those whole numbers of seconds, smallest first. For a 500 kHz/s
    station sampled at 40 kHz it is every odd second, since
    500000 mod 40000 = 20000 = sr/2 exactly.
    """
    out = []
    for k in range(1, limit + 1):
        if abs(((rate * k) % sr) - sr / 2.0) < sr * 0.02:
            out.append(k)
    return out


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

    # What the dechirp can represent at all. Beats fold at +-sr/2, so the
    # range axis is unambiguous only over +-c*(sr/2)/rate -- and the archive
    # keeps a window of that, not the whole of it. A trace outside the stored
    # window is not faint here, it is absent, and a profile read without
    # knowing that invites exactly the wrong conclusion.
    h_max = 300e3 / meta["rate"] * (meta["sr"] / 2.0)
    covered = ranges_km[-1] - ranges_km[0]
    print("  unambiguous  +-%.0f km; this archive keeps %.0f to %.0f km (%.0f%%)"
          % (h_max, ranges_km[0], ranges_km[-1], 100.0 * covered / (2 * h_max)))

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
    top = picked[0]

    # Is the strongest thing a peak, or the edge of the window?
    #
    # A discrete echo falls away on both sides. A profile that is still
    # climbing when it runs out of window is the skirt of something beyond
    # it, and calling that "an echo at 7966 km" is worse than saying nothing:
    # it is a real feature reported at the one range it certainly is not at.
    # Judged on the REGION, not on the single strongest gate.
    #
    # First version asked only whether the argmax sat in the last few gates,
    # and so called a profile whose six strongest peaks were all between
    # 10000 and 12000 km -- on both signs, which is the fold seen from both
    # ends -- "a real echo at 10373 km". Aliased energy piles into a broad
    # band near +-h_max and need not peak in the final gate.
    near_fold = np.abs(ranges_km) > 0.85 * h_max
    in_fold = sum(1 for i in picked if near_fold[i])
    at_edge = in_fold >= max(2, (len(picked) + 1) // 2)
    # "Rising" now means the fold band really is above the rest of the axis,
    # which is what separates it from a flat profile that happens to have its
    # largest wiggle out there.
    rising = False
    if at_edge and near_fold.any() and (~near_fold).any():
        rising = np.median(excess_db[near_fold]) > np.median(excess_db[~near_fold]) + 0.5

    print()
    # Is there any "outside" left? When the window is already the whole
    # unambiguous range the energy is not beyond the edge, it is ON the fold,
    # and telling the operator to widen a full window is advice that cannot
    # be followed.
    whole = covered >= 2 * h_max - 2 * abs(ranges_km[1] - ranges_km[0])
    if at_edge and rising and best >= 1.0 and whole:
        folds = fold_seconds(meta["rate"], meta["sr"])
        print("  The energy is piled on the fold at +-%.0f km, and this window"
              % h_max)
        print("  is already the whole unambiguous range. So it is not")
        print("  somewhere else: it is at the one range that means aliased.")
        print()
        print("  A chirptime wrong by dt leaves a constant beat of rate*dt,")
        print("  which folds onto +-sr/2 for these whole-second errors:")
        if folds:
            print("      dt = %s s"
                  % ", ".join("%+d" % k for k in folds[:4]))
            print("  so try chirptime %+d and %+d from its present value."
                  % (-folds[0], folds[0]))
        else:
            print("      none at a whole second -- suspect the sweep rate or")
            print("      the sample rate rather than the start time.")
        print()
        # Said here because otherwise the weakness reads as a second,
        # separate fault, and it is the same one.
        ratio = meta["rate"] / meta["sr"]
        if ratio > 1.0:
            print("  Expect it weak as well as misplaced: %.0f kHz off, the"
                  % (meta["rate"] / 1e3))
            print("  decimating boxcar is down about %.0f dB -- close to the"
                  % (20.0 * math.log10(math.pi * ratio)))
            print("  %.1f dB here against the ~20 dB of a clean echo." % best)
        print()
        # The other cause of the same picture, and the one that cost a whole
        # evening: a transmitter that sweeps for 60 s of every 300 leaves a
        # window placed in its silence holding nothing but aliased
        # interference, which also piles onto the fold. The two look
        # identical in one profile and differ in one respect only.
        print("  But measure this before believing it: if the fold reading")
        print("  does NOT change when chirptime does, it is not the signal at")
        print("  all -- broadband interference aliases onto the fold too, and")
        print("  a transmitter silent during the window looks exactly like")
        print("  this. Check the minute your captures start against the")
        print("  minute the source publishes the sweep at.")
    elif at_edge and rising and best >= 1.0:
        far = ranges_km[-1] if top >= n // 2 else ranges_km[0]
        print("  The profile is still climbing where the window ends (%.0f km)."
              % far)
        print("  That is the skirt of something OUTSIDE the stored range, not")
        print("  an echo at the edge -- a real echo falls away on both sides.")
        print()
        print("  Rebuild one capture across the whole +-%.0f km and look again:"
              % h_max)
        # "=" and not a space; see the note on the other copy of this line.
        print("      python3 python/lfp_products.py <capture>.lfs --force "
              "--h5-range-km=-%.0f,%.0f" % (h_max, h_max))
        print("      python3 tools/find_trace.py <the new .h5>")
    elif best < 1.0:
        print("  Nothing stands above the noise anywhere in %.0f-%.0f km."
              % (ranges_km[0], ranges_km[-1]))
        print("  This is not a settings fault: with the wrong chirptime or the")
        print("  wrong coordinates a trace still appears, just in the wrong")
        print("  place. A flat profile means nothing was received -- the")
        print("  transmitter is off, out of propagation, or too weak here.")
        if covered < 2 * h_max - 1.0:
            print()
            print("  Note this window is %.0f%% of the unambiguous +-%.0f km."
                  % (100.0 * covered / (2 * h_max), h_max))
            print("  Widen it before concluding anything:")
            # "=" and not a space: argparse reads a bare -12000,12000 as an
            # option name, not as this option's value.
            print("      python3 python/lfp_products.py <capture>.lfs --force "
                  "--h5-range-km=-%.0f,%.0f" % (h_max, h_max))
    elif best < 3.0:
        print("  Marginal: %.1f dB is not convincing on its own. Re-run over a"
              % best)
        print("  day's archives before concluding anything.")
    else:
        print("  %.1f dB at %.0f km is a real echo."
              % (best, ranges_km[top]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

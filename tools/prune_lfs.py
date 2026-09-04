#!/usr/bin/env python3
"""Delete raw .lfs captures whose derived products exist.

    python3 prune_lfs.py ~/ionograms                 # dry run, says what it would do
    python3 prune_lfs.py ~/ionograms --apply
    python3 prune_lfs.py ~/ionograms --keep-days 7 --free-gb 40 --apply

Why not simply "delete anything older than N days"
--------------------------------------------------
Because age is not the constraint; the disk is. A fixed window either wastes
space when the archive is small or overruns when it is not, and the operator
finds out which only when a sounding fails for want of room.

So this takes two numbers and deletes oldest-first:

  --keep-days   a FLOOR. Captures newer than this are never deleted, whatever
                the disk looks like. This is the reprocessing window: how far
                back you can still change fft_count or re-derive the gate.
  --free-gb     a CEILING. Nothing is deleted at all while there is already
                this much free. Only when free space falls below it does
                pruning start, and it stops as soon as the target is met.

With the defaults, a disk with room does nothing and a disk under pressure
frees exactly as much as it needs, oldest first. Set --free-gb 0 to prune
purely by age.

What must be true before a capture is deleted
---------------------------------------------
Both derived products must exist, be newer than the capture, and -- for the
HDF5 -- actually open and carry the datasets that make it usable. An .h5 that
was half-written when the machine lost power is worse than none, because it
looks like a successful archive.

A capture with no .h5 is never deleted, even when it is ancient. Pruning
without the archive is the one irreversible mistake here: the .lfp keeps only
the gated dB array, so a capture pruned against a sidecar alone can never be
reprocessed at a different gate, window or FFT length.
"""

import argparse
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))

REQUIRED_H5 = ("SNR", "freqs", "ranges", "rate", "t0", "sr")


def sidecar_of(lfs):
    return os.path.splitext(lfs)[0] + ".lfp"


def archive_of(lfs):
    """The .h5 beside a capture.

    Found by scanning rather than by name: the archive is named for the epoch
    in its header (`lfm_ionogram-tx-rx-ch0-000-<t0>.h5`) while the capture is
    named for its UTC wall clock, so the two names cannot be derived from each
    other without re-reading the header.
    """
    import h5_archive
    import lfp_products

    day = os.path.dirname(lfs)
    try:
        header = lfp_products.read_lfs_header(lfs)
    except Exception:
        return None
    meta = {"tx_name": header["tx_name"], "rx_name": header["rx_name"],
            "start_epoch": header["start_epoch"]}
    candidate = os.path.join(day, h5_archive.archive_name(meta))
    return candidate if os.path.exists(candidate) else None


def archive_is_sound(path):
    """Does this .h5 open, and does it carry what a reader needs?

    Existence is not enough. A file truncated by a power cut still has a name
    and a size, and deleting an 80 MB capture against it loses the sounding for
    good.
    """
    try:
        import h5py
    except ImportError:
        return False, "h5py missing, cannot verify"
    try:
        with h5py.File(path, "r") as fh:
            missing = [k for k in REQUIRED_H5 if k not in fh]
            if missing:
                return False, "missing %s" % ", ".join(missing)
            snr = fh["SNR"]
            if snr.ndim != 2 or 0 in snr.shape:
                return False, "SNR is %s" % (snr.shape,)
            if snr.shape != (fh["freqs"].size, fh["ranges"].size):
                return False, "SNR %s against axes (%d, %d)" % (
                    snr.shape, fh["freqs"].size, fh["ranges"].size)
    except Exception as exc:
        return False, "unreadable: %s" % exc
    return True, ""


def scan(root):
    """Every .lfs under root, oldest first by mtime."""
    found = []
    for where, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(".lfs"):          # never .lfs.partial
                path = os.path.join(where, name)
                try:
                    found.append((os.path.getmtime(path), path,
                                  os.path.getsize(path)))
                except OSError:
                    pass
    found.sort()
    return found


def classify(lfs, mtime, keep_before):
    """Why this capture may or may not be deleted."""
    if mtime > keep_before:
        return False, "inside the keep window"

    side = sidecar_of(lfs)
    if not os.path.exists(side):
        return False, "no .lfp"
    if os.path.getmtime(side) < mtime:
        return False, ".lfp older than the capture"

    arc = archive_of(lfs)
    if arc is None:
        return False, "no .h5 archive"
    if os.path.getmtime(arc) < mtime:
        return False, ".h5 older than the capture"
    sound, why = archive_is_sound(arc)
    if not sound:
        return False, ".h5 %s" % why

    return True, ""


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="the ionograms directory")
    ap.add_argument("--keep-days", type=float, default=7.0,
                    help="never delete captures newer than this (default 7). "
                         "This is the reprocessing window.")
    ap.add_argument("--free-gb", type=float, default=50.0,
                    help="do nothing while at least this much is free; below "
                         "it, delete oldest-first until it is met again "
                         "(default 50). 0 prunes purely by age.")
    ap.add_argument("--apply", action="store_true",
                    help="actually delete. Without it nothing is removed.")
    opts = ap.parse_args()

    root = os.path.expanduser(opts.archive)
    if not os.path.isdir(root):
        sys.stderr.write("%s is not a directory\n" % root)
        return 2

    usage = shutil.disk_usage(root)
    free_gb = usage.free / 1e9
    want_gb = opts.free_gb

    print("=" * 68)
    print(" pruning %s" % root)
    print("=" * 68)
    print("  disk    %.1f GB free of %.1f GB" % (free_gb, usage.total / 1e9))
    print("  keep    everything from the last %.1f days" % opts.keep_days)
    if want_gb > 0:
        print("  target  %.1f GB free" % want_gb)
    else:
        print("  target  none; pruning by age alone")

    captures = scan(root)
    total_gb = sum(c[2] for c in captures) / 1e9
    print("  found   %d captures, %.1f GB" % (len(captures), total_gb))

    if want_gb > 0 and free_gb >= want_gb:
        print()
        print("  Nothing to do: already above the target with %.1f GB to spare."
              % (free_gb - want_gb))
        return 0

    keep_before = time.time() - opts.keep_days * 86400.0
    need_gb = (want_gb - free_gb) if want_gb > 0 else float("inf")

    freed = 0.0
    removed = 0
    blocked = {}
    print()
    for mtime, path, size in captures:
        if want_gb > 0 and freed >= need_gb:
            break
        ok, why = classify(path, mtime, keep_before)
        if not ok:
            blocked[why] = blocked.get(why, 0) + 1
            continue

        gb = size / 1e9
        age = (time.time() - mtime) / 86400.0
        if opts.apply:
            try:
                os.remove(path)
            except OSError as exc:
                print("  *** %s: %s" % (os.path.basename(path), exc))
                continue
        print("  %-44s %5.1f d  %6.1f MB%s"
              % (os.path.basename(path), age, size / 1e6,
                 "" if opts.apply else "   [dry run]"))
        freed += gb
        removed += 1

    print()
    print("  %s %d capture%s, %.2f GB"
          % ("removed" if opts.apply else "would remove", removed,
             "" if removed == 1 else "s", freed))
    if blocked:
        print()
        print("  kept:")
        for why in sorted(blocked, key=lambda k: -blocked[k]):
            print("    %-34s %d" % (why, blocked[why]))

    if want_gb > 0 and freed < need_gb:
        print()
        print("  *** still %.1f GB short of the target." % (need_gb - freed))
        print("  *** Everything else is either inside the keep window or has")
        print("  *** no sound .h5 archive. Lower --keep-days, or check that")
        print("  *** h5_archive is enabled in the sounder's config.")

    if not opts.apply and removed:
        print()
        print("  Nothing was deleted. Re-run with --apply.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

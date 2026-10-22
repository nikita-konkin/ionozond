#!/usr/bin/env python3
"""Re-cut the echo windows that kept the whole band by accident.

    python3 recut_windows.py /mnt/ionozond_5tb/ionozond_data            # dry run
    python3 recut_windows.py /mnt/ionozond_5tb/ionozond_data --apply

Before issue 4, the echo window's auto gate could open on a chance noise peak
or on the band edges, far from any echo. The window then spanned too much of
the band to decimate, and the archive kept all of it: 65 MB instead of 3.

Such a window is the capture sample for sample, only mixed down by its
shift: one tap, no decimation. So the capture can be rebuilt from it, and is,
into a scratch folder; the rebuild is checked by mixing it down again, which
must give the stored voltage back. It is then cut again by today's auto gate,
into a copy of the archive without its old window, and verified against the
rebuilt capture as the sounder verifies every window. Only then does the copy
replace the archive, keeping its time; its power spectrogram is copied
untouched.

A window kept whole on purpose -- save_raw_voltage, recorded as a manual
range -- is left alone, and so is every decimated one.

What the new window leaves out of the band is gone once --apply replaces the
file: the window was the last copy of the capture.
"""

import argparse
import json
import os
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))

import echo_window  # noqa: E402
import h5_archive  # noqa: E402

# How far the rebuilt capture, mixed down again, may stray from the stored
# voltage: complex64 rounding, once each way, is about 1e-7.
REBUILD_TOLERANCE = 1e-5


def archives(paths):
    """Every .h5 under `paths`, which may name files or folders, in order."""
    found = []
    for path in paths:
        if os.path.isfile(path):
            found.append(path)
        for folder, _dirs, names in os.walk(path):
            found.extend(os.path.join(folder, n) for n in names)
    return sorted(set(p for p in found if p.endswith(".h5")))


def whole(path):
    """Does the archive hold a window that kept the whole band by accident?

    One tap and no decimation is the whole band. A manual range is
    save_raw_voltage asking for it.
    """
    h5py = echo_window._h5py()
    with h5py.File(path, "r") as fh:
        if echo_window.GROUP not in fh:
            return False
        group = fh[echo_window.GROUP]
        selection = json.loads(echo_window._text(group.attrs["selection"]))
        return (int(group.attrs["decimation"]) == 1
                and group[echo_window.TAPS].shape == (1,)
                and selection.get("method") != "manual")


def rebuild(window, lfs):
    """Write the capture a whole-band window holds back out as an .lfs."""
    x = echo_window.reconstruct(window).astype(np.complex64)
    again = echo_window.cut(x, {"source_sr_hz": window["source_sr_hz"],
                                "shift_hz": window["shift_hz"],
                                "decimation": 1, "numtaps": 1,
                                "taps": window["taps"]})
    z = window["z"]
    error = np.sqrt(np.mean(np.abs(again - z) ** 2) / max(np.mean(np.abs(z) ** 2), 1e-300))
    if error > REBUILD_TOLERANCE:
        raise ValueError("the rebuilt capture misses the stored voltage by %.1e" % error)
    with open(lfs, "wb") as fh:
        fh.write(window["lfs_header"])
        fh.write(x.tobytes())


def strip(path, copy):
    """Copy an archive, attributes and all, without its echo window."""
    h5py = echo_window._h5py()
    with h5py.File(path, "r") as src, h5py.File(copy, "w") as dst:
        for key, value in src.attrs.items():
            dst.attrs[key] = value
        for key in src:
            if key != echo_window.GROUP:
                src.copy(src[key], dst, name=key)
    ok, why = h5_archive.is_sound(copy)
    if not ok:
        raise ValueError("the copy without the window is not sound (%s)" % why)


def recut(path, scratch):
    """Replace the whole-band window of `path` with today's cut of it.

    Returns echo_window.write's report. Raises, leaving the archive as it
    was, when anything on the way fails.
    """
    if not os.path.isdir(scratch):
        os.makedirs(scratch)
    window = echo_window.read(path)
    lfs = os.path.join(scratch, window["source_name"])
    copy = os.path.join(os.path.dirname(os.path.abspath(path)),
                        ".%s.recut" % os.path.basename(path))
    stat = os.stat(path)
    try:
        rebuild(window, lfs)
        del window
        strip(path, copy)
        report = echo_window.write(lfs, into=copy, check=True)
        os.utime(copy, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        os.replace(copy, path)
    finally:
        for name in (lfs, copy):
            if os.path.exists(name):
                os.remove(name)
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help=".h5 archives, or folders to search")
    ap.add_argument("--apply", action="store_true",
                    help="re-cut and replace; without it, only say what would be")
    ap.add_argument("--scratch",
                    help="folder for the rebuilt captures, 75 MB at a time "
                         "(default: a temporary one)")
    args = ap.parse_args(argv)
    scratch = args.scratch or tempfile.mkdtemp(prefix="recut_windows-")
    found = before = after = failed = 0
    for path in archives(args.paths):
        try:
            if not whole(path):
                continue
        except Exception as exc:
            print("%s: unreadable, left alone (%s)" % (path, exc))
            failed += 1
            continue
        found += 1
        size = os.path.getsize(path)
        before += size
        if not args.apply:
            print("%s: whole band, %.1f MB" % (path, size / 1e6))
            continue
        try:
            report = recut(path, scratch)
        except Exception as exc:
            print("%s: left alone, the re-cut failed (%s)" % (path, exc))
            after += size
            failed += 1
            continue
        after += os.path.getsize(path)
        print("%s: %.1f -> %.1f MB, window %.0f..%.0f km (/%d, %s)"
              % (path, size / 1e6, os.path.getsize(path) / 1e6, report["lo_km"],
                 report["hi_km"], report["decimation"], report["method"]))
    if not args.scratch:
        os.rmdir(scratch)
    if args.apply:
        print("%d whole-band windows, %d re-cut, %.2f GB freed"
              % (found, found - failed, (before - after) / 1e9))
    else:
        print("%d whole-band windows in %.2f GB; --apply re-cuts them"
              % (found, before / 1e9))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

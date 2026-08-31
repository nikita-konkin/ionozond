#!/usr/bin/env python3
"""Write a capture as chirpsounder2's ``lfm_ionogram-*.h5``.

The point is to be able to delete the ``.lfs``. A capture is 80 MB and the
station writes 288 a day -- 23 GB, 691 GB a month -- while the ``.lfp`` sidecar
beside it is 70 kB. But the sidecar is lossy and terminal: its IONO section is
the *gated* dB array, thresholded and despeckled and windowed to +-1500 km, so
once the raw file is gone there is no re-tuning obj_level, no switching to
``--iono-mode snr``, and no reprocessing after a DSP fix.

This is the middle term. A windowed, median-normalised power spectrogram at
about 1.25 MB: 64x smaller than the capture, 25x larger than the sidecar, and
enough to rebuild any ionogram product that does not need a different FFT
length.

What it cannot do, and the archive is not a substitute for the capture in
these respects: change `fft_count`, since the time/frequency trade is baked in
at write time; re-derive the Rosin gate, which power_dynamic_limit computes
over the *whole* spectrum and the archive keeps only a window of; or anything
needing the complex time series, so no Doppler, coherent integration or O/X
separation.

Format
------
chirpsounder2's, so that ionograms-handler reads our captures with no new code
-- its ``muf/io_chirp.py`` already parses this, and ``muf/loader.py`` dispatches
on the ``lfm_ionogram-`` filename prefix. The schema is flat at the root group;
``REQUIRED = ("SNR", "freqs", "ranges", "rate", "t0", "sr")``.

The one non-obvious mapping is SNR. ``io_chirp.snr_to_power`` returns
``max(SNR + 1, 0) / NOISE_COEF``, and ``muf/spectro.py`` computes the same
quantity straight from a ``.lfs`` as ``row / (NOISE_COEF * median(row))``. Our
``build_spectra`` has already divided each spectrum by its own median, so

    SNR = normalised power - 1

makes the two paths numerically identical. That equality is the whole claim of
this module and ``--verify`` checks it.
"""

import math
import os
import subprocess

import numpy as np

# 3e5, not the exact 299792.458.
#
# chirpsounder2 uses scipy's c, but this whole system does not: lfp_products.py
# and src/common.h both define LIGHT_SPEED_KM_S = 300e3, and so does
# ionograms-handler's own .lfs reader (muf/calibrate.py:28, C_KM_S = 3e8/1e3).
# Every delay axis the station has ever produced is calibrated against 3e5.
#
# Using the exact value here would be more correct in the abstract and wrong in
# practice: it puts the archive's range axis 41 km from the .lfp's at the edge
# of the span, so the same echo would read differently depending on which file
# you opened. Matching the rest of the system is worth more than the 0.07%.
LIGHT_SPEED_KM_S = 300e3

# What we claim to be. No reader dispatches on it -- it is provenance only --
# but every chirpsounder2 product carries it and a file without it looks odd.
SCHEMA_VERSION = "0.2.0"

# None: store every cell.
#
# chirpsounder2 NaNs out everything below `storage_snr_threshold` (default 2.0
# linear) because deflate then collapses the background -- "can be 90%
# savings". Tempting, but the discarded cells do not come back: io_chirp reads
# NaN as the row median, so a thresholded archive returns a constant where the
# noise field was.
#
# Measured on a real capture, keeping everything costs 1.25 MB against 0.88 at
# threshold 0 -- 42% more for the complete field, still 64x smaller than the
# 80 MB capture. And threshold 0 is not the mild choice it looks: SNR = P - 1,
# so it discards every cell below the noise median, which is half of them.
# For an archive whose purpose is reprocessing, 0.37 MB is not worth half the
# distribution.
DEFAULT_SNR_THRESHOLD = None

# The delay window to keep, in km of one-way group path. 0-8000 covers 1F
# through 3F on the Cyprus circuit (9.0, 9.7, 10.6 ms) with room to spare, and
# is far wider than the +-1500 km the console displays.
DEFAULT_RANGE_KM = (0.0, 8000.0)


def _h5py():
    """Imported late, and with a usable error, as rx_dechirp does for numpy."""
    try:
        import h5py
    except ImportError:
        raise RuntimeError(
            "h5py is missing:  sudo apt-get install -y python3-h5py")
    return h5py


def range_axis(header, fft_count):
    """Every range gate the FFT produces, km, ascending.

    Bin k of an N-point FFT sits at exactly k*sr/N, so the gate spacing is
    (c/rate)*(sr/N). Note this is NOT what geometry() uses for the display
    window -- that divides the span by fft_count-1, treating the axis as
    symmetric and inclusive at both ends. Over a 60000 km half-span the two
    differ by 3.7 km at the edge, which is half a delay bin.

    The FFT's version is the correct one and is what chirpsounder2 writes
    (`fftshift(fftfreq(fftlen, 1/sr)) * c/rate`), so the archive uses it. The
    console's off-by-one is a separate, pre-existing matter; correcting it
    there would move every stored .lfp axis.

    Ordering follows build_spectra, which fftshifts and then reverses: after
    the reversal row 0 holds the most negative range and the last row the most
    positive, and since beat = -rate*tau the first gate is -h_max + step.
    """
    if_rate = header["sample_rate"] / float(header["dec"])
    scale = LIGHT_SPEED_KM_S / header["rate"]      # km per Hz of beat
    step = scale * (if_rate / fft_count)
    h_max = scale * (if_rate / 2.0)
    return -h_max + step * (np.arange(fft_count, dtype=np.float64) + 1.0), step


def window_rows(header, fft_count, range_km=DEFAULT_RANGE_KM):
    """Rows of the spectrum covering `range_km`, as a half-open (lo, hi)."""
    axis, _step = range_axis(header, fft_count)
    lo = int(np.searchsorted(axis, range_km[0], side="left"))
    hi = int(np.searchsorted(axis, range_km[1], side="right"))
    lo = max(0, min(lo, fft_count - 1))
    hi = max(lo + 1, min(hi, fft_count))
    return lo, hi


def freq_axis(header, fft_count, spec_count):
    """Centre frequency of each spectrum, Hz, ascending.

    One spectrum spans fft_count/if_rate seconds of sweep, so the transmitter
    has moved rate * that between columns.
    """
    if_rate = header["sample_rate"] / float(header["dec"])
    first = header["cf"] - header["sample_rate"] / 2.0
    step = header["rate"] * (fft_count / if_rate)
    # Centres, not starts. A spectrum integrates fft_count/if_rate seconds, so
    # the transmitter was at the mid-point of that span on average; labelling
    # it with the start puts every column half a bin -- 20.5 kHz -- low.
    return first + step * (np.arange(spec_count, dtype=np.float64) + 0.5)


def _git_metadata():
    """Provenance, best effort. Absent is fine -- io_chirp guards every read."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        commit = subprocess.check_output(
            ["git", "-C", here, "rev-parse", "--short=12", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
        dirty = subprocess.call(
            ["git", "-C", here, "diff", "--quiet", "HEAD"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0
        return commit, dirty
    except Exception:
        return None, None


def archive_name(meta, channel="ch0", chirp_id=0):
    """chirpsounder2's filename. The prefix is load-bearing: muf/loader.py
    globs `lfm_ionogram-*.h5` and will not see anything else.

    The station names go in as-is even though `yoshkar-ola` contains a hyphen
    and would confuse io_chirp's filename regex. That regex is only a fallback
    for when the txname/station_name datasets are missing, and we always write
    them, so it never runs.
    """
    return "lfm_ionogram-%s-%s-%s-%03d-%.2f.h5" % (
        meta["tx_name"], meta["rx_name"], channel, int(chirp_id),
        float(meta["start_epoch"]))


def write(path, meta, snr, freqs_hz, ranges_km,
          channel="ch0", chirp_id=0, threshold=DEFAULT_SNR_THRESHOLD):
    """Write one ionogram. `snr` is (n_freq, n_range), normalised power - 1."""
    h5py = _h5py()

    snr = np.asarray(snr, dtype=np.float32)
    if snr.shape != (len(freqs_hz), len(ranges_km)):
        raise ValueError("SNR is %s, expected (%d, %d)"
                         % (snr.shape, len(freqs_hz), len(ranges_km)))

    # Sub-threshold cells become NaN, which is what lets deflate collapse the
    # background -- but io_chirp reads NaN back as the row median, so anything
    # dropped here is gone. None keeps the lot; see DEFAULT_SNR_THRESHOLD.
    if threshold is None:
        stored = snr.astype(np.float16)
    else:
        stored = snr.copy()
        stored[stored < threshold] = np.nan
        stored = stored.astype(np.float16)

    ranges_m = np.asarray(ranges_km, dtype=np.float64) * 1e3

    tmp = path + ".part"
    with h5py.File(tmp, "w") as fh:
        fh.attrs["chirpsounder2_version"] = SCHEMA_VERSION
        commit, dirty = _git_metadata()
        if commit is not None:
            fh.attrs["git_commit"] = commit
            fh.attrs["git_dirty"] = bool(dirty)
        fh.attrs["producer"] = "ionozond"

        fh.create_dataset("SNR", data=stored, compression="gzip",
                          compression_opts=9, shuffle=True)
        fh["freqs"] = np.asarray(freqs_hz, dtype=np.float64)
        fh["ranges"] = ranges_m
        fh["rate"] = float(meta["rate"])
        fh["sr"] = float(meta["sample_rate"]) / float(meta["dec"])
        fh["t0"] = float(meta["start_epoch"])

        # True, because our range axis is already absolute one-way range from
        # the transmitter. With False, io_chirp adds (t0 - floor(t0)) * c to
        # every gate -- zero for us today, since start_epoch is a whole second,
        # but the wrong flag the moment sub-second starts appear.
        fh["range_offset_applied"] = True
        fh["range_gate_start_m"] = float(ranges_m[0])
        fh["range_gate_stop_m"] = float(ranges_m[-1])

        fh["txname"] = meta["tx_name"]
        fh["station_name"] = meta["rx_name"]
        fh["ch"] = channel
        fh["id"] = int(chirp_id)

        # Ours is 1 by construction: build_spectra divides each spectrum by its
        # own median. Recorded because the name means "the noise floor of the
        # stored data", and for us that is exactly what it is.
        fh["noise_floor"] = np.ones(len(freqs_hz), dtype=np.float64)

    os.replace(tmp, path)
    return os.path.getsize(path)


def read(path):
    """Read one back, as io_chirp does. For --verify and for our own tools."""
    h5py = _h5py()
    with h5py.File(path, "r") as fh:
        def scalar(key):
            v = fh[key][()]
            return v.decode() if isinstance(v, bytes) else v
        return {
            "snr": np.asarray(fh["SNR"][()], dtype=np.float32),
            "freqs_hz": np.asarray(fh["freqs"][()], dtype=np.float64),
            "ranges_m": np.asarray(fh["ranges"][()], dtype=np.float64),
            "rate": float(scalar("rate")),
            "sr": float(scalar("sr")),
            "t0": float(scalar("t0")),
            "tx_name": str(scalar("txname")) if "txname" in fh else "",
            "rx_name": str(scalar("station_name")) if "station_name" in fh else "",
            "offset_applied": bool(scalar("range_offset_applied")),
        }


def power_from_snr(snr):
    """What io_chirp.snr_to_power does, so our tests can assert the identity.

    NOISE_COEF is 4*ln2, not the 2*ln2 our own gate uses -- see
    docs/reverse-engineering.md on why the two differ and why theirs is kept
    here.
    """
    noise_coef = 4.0 * math.log(2.0)
    finite = np.nan_to_num(np.asarray(snr, dtype=np.float32),
                           nan=0.0, posinf=np.float32(65504.0), neginf=-1.0)
    return np.maximum(finite + 1.0, 0.0) / noise_coef

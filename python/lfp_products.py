#!/usr/bin/env python3
"""Compute the derived products of an .lfs capture and write the .lfp sidecar.

    python3 lfp_products.py cyprus1_20260829_142010.lfs
    python3 lfp_products.py <archive-dir> --recurse
    python3 lfp_products.py capture.lfs --verify

The sidecar is what the console, the archive browser and the trend plots
actually read; the 80 MB capture only has to be processed once. See
docs/lfp-format.md for the format and docs/reverse-engineering.md for where
each stage of the pipeline came from.

This is the same pipeline as src/qrxionogram.cpp, stage for stage, in numpy --
so a station can produce sidecars without Qt, and the sounder can write one in
the gap between soundings.
"""

import argparse
import math
import os
import struct
import sys
import zlib

import numpy as np

# ---------------------------------------------------------------- constants
# All from src/common.h, recovered from the original binary.
EARTH_RADIUS_KM = 6378.137
LAY_E_SQR = 40000.0                  # (2 x 100 km) -- reflection at 100 km
LIGHT_SPEED_KM_S = 300e3
MS_IN_SECOND = 1000.0
VIRT_HEIGHT_WINDOW_KM = 1500.0
VIRT_HEIGHT_MARGIN_KM = 180.0
HZ_IN_MHZ = 1e6

OBJ_SIZE_HORIZONTAL = 9              # deleteSmallObjects window, spectra
OBJ_SIZE_VERTICAL = 3                # ... and delay rows
OBJ_LEVEL = 11.0                     # neighbours needed to survive
# The original's noise constant, recovered from the binary. Its usual gloss --
# "median to mean for an exponential" -- is not what it is: that conversion is
# 1/ln2 = 1.4427, since the median of an exponential is mean*ln2. 2*ln2 lands
# 3.9% below it (-0.17 dB), close by the coincidence that 2*ln^2(2) = 0.961
# rather than by derivation.
#
# What it does do is set a threshold 1.42 dB above the median, and for
# exponential noise the fraction of pure-noise cells surviving a threshold of
# F * median is exactly 2^-F. At F = 2*ln2 that is 38.3%, so this gate passes
# well over a third of the noise and the Rosin threshold and speckle filter
# downstream do the real work. Kept at the original's value regardless: this is
# a reconstruction, and matching it matters more than improving it.
NOISE_FACTOR = 1.3862943611198906    # 2*ln(2), the original's

# What the IONO section holds. See compute().
IONO_MODE_GATED = "gated"            # the original: threshold, despeckle, zero
IONO_MODE_SNR = "snr"                # ionograms-handler: continuous, nothing cut

# Reproducing ionograms-handler's scale exactly, from muf/spectro.py and
# muf/render.py. Its numbers only mean anything together: the noise floor is
# divided out with a 4*ln(2) coefficient, the log is referenced to 1e-3 rather
# than to 1, and the display window is 20..75 dB. Referencing to 1e-3 puts
# 0 dB thirty decibels below unity, so the equalised noise floor lands near
# 26 dB and a window starting at 20 keeps a few dB of it visible.
#
# Note this coefficient is not ours. NOISE_FACTOR above is 2*ln(2); this is
# exactly twice that, and 2.84 dB above the 1/ln(2) that actually converts an
# exponential's median to its mean. As a divisor, which is how it is used here,
# that shifts every value down uniformly and cancels against the fixed 20..75
# window -- so it is kept as theirs, their thresholds being calibrated against
# it. As a threshold it would be a different matter: 14.6% of noise surviving
# against our 38.3%.
IONO_SNR_NOISE_COEF = 4.0 * math.log(2.0)   # muf/spectro.py NOISE_COEF
IONO_SNR_DB_REF = 1e-3                      # muf/spectro.py to_db floor
IONO_SNR_VMIN_DB = 20.0                     # muf/render.py DEFAULT_VMIN_DB
IONO_SNR_VMAX_DB = 75.0                     # muf/render.py DEFAULT_VMAX_DB

LFS_HEADER_SIZE = 512
LFP_MAGIC = b"LFPR"
LFP_HEADER_SIZE = 512
LFP_SECTION_ENTRY = 32
LFP_VERSION = (1, 0)

_LFS_STRUCT = "<4sf4sH64sff64sff7HIIIHIIiiIIHII292s"


# ---------------------------------------------------------------- geometry

def earth_distance_km(lat1, lon1, lat2, lon2):
    """Great-circle distance, matching src/common.cpp exactly.

    Note the coordinates are fed in as stored. The archive stores degrees and
    minutes as DD.MM while every reader treats them as decimal degrees -- see
    the warning in docs/lfs-format.md. Reproduced here so the sidecar's delay
    window matches what the console draws.
    """
    la1, lo1, la2, lo2 = (math.radians(v) for v in (lat1, lon1, lat2, lon2))
    dlon = lo2 - lo1
    x = math.sin(la1) * math.sin(la2) + math.cos(la1) * math.cos(la2) * math.cos(dlon)
    y = math.hypot(math.cos(la2) * math.sin(dlon),
                   math.cos(la1) * math.sin(la2) -
                   math.sin(la1) * math.cos(la2) * math.cos(dlon))
    return EARTH_RADIUS_KM * math.atan2(y, x)


def ray_distance_km(ground_km):
    return math.sqrt(LAY_E_SQR + ground_km * ground_km)


def time_ms_from_height_km(km):
    return MS_IN_SECOND * (km / LIGHT_SPEED_KM_S)


def read_lfs_header(path):
    with open(path, "rb") as fh:
        raw = fh.read(LFS_HEADER_SIZE)
    if len(raw) < LFS_HEADER_SIZE or raw[:4] != b"LFSG":
        raise ValueError("%s is not an .lfs capture" % path)
    f = struct.unpack(_LFS_STRUCT, raw)
    return {
        "format_ver": f[1], "header_size": f[3],
        "tx_name": f[4].split(b"\0")[0].decode("latin-1"),
        "tx_lat": f[5], "tx_lon": f[6],
        "rx_name": f[7].split(b"\0")[0].decode("latin-1"),
        "rx_lat": f[8], "rx_lon": f[9],
        "start_year": f[10], "start_daynumber": f[11], "start_month": f[12],
        "start_day": f[13], "start_hour": f[14], "start_minute": f[15],
        "start_second": f[16], "start_epoch": f[17],
        "chirpt": f[18], "cf": f[19], "dur": f[20], "rate": f[21],
        "rep": f[22], "rmin": f[23], "rmax": f[24],
        "dec": f[25], "sample_rate": f[26],
        "whiten": f[27], "whiten_len": f[28], "whiten_n": f[29],
    }


def geometry(h, fft_count, sample_count):
    """Axes and the delay window, as src/qrxionogram.cpp computes them."""
    if_rate = h["sample_rate"] / float(h["dec"])
    freq_min = (h["cf"] - h["sample_rate"] / 2.0) / HZ_IN_MHZ
    freq_max = freq_min + h["dur"] * h["rate"] / HZ_IN_MHZ

    ground = earth_distance_km(h["tx_lat"], h["tx_lon"], h["rx_lat"], h["rx_lon"])
    ray = ray_distance_km(ground)
    rmin_km = ray - VIRT_HEIGHT_MARGIN_KM
    rmax_km = rmin_km + VIRT_HEIGHT_WINDOW_KM + VIRT_HEIGHT_MARGIN_KM

    h_max = LIGHT_SPEED_KM_S * (if_rate / 2.0) / h["rate"]
    h_min = -h_max
    km_per_row = (h_max - h_min) / float(fft_count - 1)

    row_low = max(0, int(math.floor((rmin_km - h_min) / km_per_row)))
    row_high = min(fft_count - 1, int(math.ceil((rmax_km - h_min) / km_per_row)))
    if row_high - row_low + 1 <= 1:
        raise ValueError("the delay window is empty; check tx/rx coordinates")

    return {
        "if_rate": if_rate,
        "freq_min_mhz": freq_min, "freq_max_mhz": freq_max,
        "ground_km": ground, "ray_km": ray,
        "row_low": row_low, "row_high": row_high,
        "rows": row_high - row_low + 1,
        "spec_count": sample_count // fft_count,
        "delay_min_ms": time_ms_from_height_km(h_min + row_low * km_per_row),
        "delay_max_ms": time_ms_from_height_km(h_min + row_high * km_per_row),
    }


# ---------------------------------------------------------------- the DSP

def hanning_periodic(n):
    """0.5*(1-cos(2*pi*k/n)) -- periodic, divided by n and not n-1, with the
    step narrowed to float32 exactly as calculateHanningWindow does."""
    step = np.float32(2.0 * np.pi / n)
    k = np.arange(n, dtype=np.float32)
    return (0.5 * (1.0 - np.cos((k * step).astype(np.float64)))).astype(np.float32)


def build_spectra(path, fft_count, spec_count, chunk=64):
    """Windowed FFT, power, fftshift, median-normalise, reverse.

    Yields the normalised linear power one chunk of spectra at a time so a
    250 s capture does not need its whole complex128 expansion in memory at
    once.
    """
    window = hanning_periodic(fft_count)
    with open(path, "rb") as fh:
        fh.seek(LFS_HEADER_SIZE)
        done = 0
        while done < spec_count:
            take = min(chunk, spec_count - done)
            raw = np.fromfile(fh, dtype=np.complex64, count=take * fft_count)
            got = len(raw) // fft_count
            if got == 0:
                return
            block = raw[:got * fft_count].reshape(got, fft_count)
            z = (block * window).astype(np.complex128)
            spec = np.fft.fft(z, axis=1)
            power = spec.real ** 2 + spec.imag ** 2
            power = np.fft.fftshift(power, axes=1)
            # getMedian is not a textbook median for odd n, but fft_count is a
            # power of two, and for even n it is the ordinary one.
            power /= np.median(power, axis=1, keepdims=True)
            yield power[:, ::-1]
            done += got


def power_dynamic_limit(spec_db):
    """Rosin/triangle threshold, matching igmath.cpp getPowerDynamicLimit.

    Integer-dB histogram, turned into a survival curve, then the bin furthest
    below the chord from (0, N) to (limit, 0). Ties go to the larger bin.
    """
    if spec_db.size == 0:
        return 0.0
    limit = int(math.floor(float(spec_db.max()) + 0.5))
    if limit < 2:
        return 0.0

    bins = np.floor(spec_db.astype(np.float64) + 0.5).astype(np.int64)
    np.clip(bins, 0, limit, out=bins)
    hist = np.bincount(bins, minlength=limit + 1)[:limit + 1]

    survival = np.cumsum(hist[::-1])[::-1]      # survival[c] = count >= c
    n = np.float32(spec_db.size)
    slope = np.float32(-n / np.float32(limit))

    d = np.arange(1, limit, dtype=np.float32)
    if d.size == 0:
        return 0.0
    deviation = slope * d + n - survival[1:limit].astype(np.float32)

    best = float(deviation.max())
    if best < 0.0:
        return 0.0
    # "deviation >= best" while scanning upward means the LAST index holding
    # the maximum wins.
    return float(d[np.nonzero(deviation >= np.float32(best))[0][-1]])


def delete_small_objects(data, window_w=OBJ_SIZE_HORIZONTAL,
                         window_h=OBJ_SIZE_VERTICAL, level=OBJ_LEVEL):
    """Speckle filter: keep points with enough neighbours in a 9x3 window.

    The original clamps the window to an interior band, so cells nearer an
    edge than half a window never accumulate a count and are always removed.
    That edge erosion is reproduced rather than corrected.
    """
    n, m = data.shape
    half_w = window_w // 2
    half_h = window_h // 2

    occupied = (data > 0.0).astype(np.float64)
    integral = np.zeros((n + 1, m + 1), dtype=np.float64)
    integral[1:, 1:] = occupied.cumsum(0).cumsum(1)

    xs = np.arange(n)
    ys = np.arange(m)
    x_lo = np.clip(xs - half_w, 0, n)[:, None]
    x_hi = np.clip(xs + half_w + 1, 0, n)[:, None]
    y_lo = np.clip(ys - half_h, 0, m)[None, :]
    y_hi = np.clip(ys + half_h + 1, 0, m)[None, :]

    counts = (integral[x_hi, y_hi] - integral[x_lo, y_hi]
              - integral[x_hi, y_lo] + integral[x_lo, y_lo])

    # Only the interior band ever receives a count.
    interior = np.zeros((n, m), dtype=bool)
    if n - half_w - 1 >= half_w and m - half_h - 1 >= half_h:
        interior[half_w:n - half_w, half_h:m - half_h] = True
    counts = np.where(interior, counts, 0.0)

    return np.where(counts >= level, data, 0.0).astype(np.float32)


def has_run(row, run_length=3):
    """Does the row contain `run_length` consecutive positive points?"""
    positive = row > 0.0
    if not positive.any():
        return False
    # Run lengths via the differences between False positions.
    idx = np.flatnonzero(~positive)
    padded = np.concatenate(([-1], idx, [positive.size]))
    return int((np.diff(padded) - 1).max()) >= run_length


def usage_frequencies(data):
    """LUF/MUF spectrum indices, matching calculateUsageFrequencies.

    Three consecutive spectra each holding three consecutive non-zero points.
    The forward pass backs up two to the first of the run, the backward pass
    goes forward two to the last.
    """
    spec_count = data.shape[0]
    luf = muf = -1
    if spec_count <= 2 or data.shape[1] <= 2:
        return luf, muf

    qualifies = np.fromiter((has_run(data[s]) for s in range(spec_count)),
                            dtype=bool, count=spec_count)

    run = 0
    for s in range(spec_count):
        if qualifies[s]:
            run += 1
            if run == 3:
                luf = s - 2
                break
        else:
            run = 0
    if luf < 0:
        return -1, -1

    run = 0
    for s in range(spec_count - 1, -1, -1):
        if qualifies[s]:
            run += 1
            if run == 3:
                muf = s + 2
                break
        else:
            run = 0
    return luf, muf


def compute(path, fft_count=16384, noise_gate=True, progress=None,
            obj_w=OBJ_SIZE_HORIZONTAL, obj_h=OBJ_SIZE_VERTICAL,
            obj_level=OBJ_LEVEL, iono_mode=IONO_MODE_GATED,
            archive_range_km=None):
    """Run the whole pipeline. Returns (meta, {section: array}).

    `iono_mode` decides only what the IONO section holds; LUF, MUF, SNR and
    PDP are derived from the gated array either way.

    "gated" is the original's: threshold each spectrum, delete speckle, store
    what survives and zero the rest. "snr" is chirpsounder2's, which stores
    the continuous field and lets the eye do the gating -- see
    plot_ionograms.py, which renders 10*log10((S - median)/median) over a
    fixed 0..20 dB window with nothing deleted at all. Multi-hop traces and
    the ordinary/extraordinary split live in exactly the faint structure the
    gate removes, which is why their ionograms read as sharper than ours
    despite coming off a comparable receiver.

    `archive_range_km` asks for a wide-window spectrogram alongside, returned
    as a fourth item for python/h5_archive.py to write. It rides along here
    rather than in a pass of its own because the 80 MB is streamed exactly
    once: reading it twice would double the cost of every capture to save a
    few lines.
    """
    header = read_lfs_header(path)
    samples = (os.path.getsize(path) - LFS_HEADER_SIZE) // 8
    if samples < fft_count:
        raise ValueError("%s holds %d samples, less than one %d-point spectrum"
                         % (path, samples, fft_count))
    geo = geometry(header, fft_count, samples)
    spec_count = geo["spec_count"]
    row_low, rows = geo["row_low"], geo["rows"]

    gated_db = np.zeros((spec_count, rows), dtype=np.float32)
    snr_db = (np.zeros((spec_count, rows), dtype=np.float32)
              if iono_mode == IONO_MODE_SNR else None)
    window_linear = np.zeros((spec_count, rows), dtype=np.float64)
    limits = np.zeros(spec_count, dtype=np.float32)
    win_max = 0.0
    at = 0

    arc_lo = arc_hi = 0
    archive = None
    if archive_range_km is not None:
        import h5_archive
        arc_lo, arc_hi = h5_archive.window_rows(header, fft_count,
                                                archive_range_km)
        # float16 because that is what the format stores and what the reader
        # casts to anyway; holding it wider here would only round later.
        archive = np.zeros((spec_count, arc_hi - arc_lo), dtype=np.float16)

    for block in build_spectra(path, fft_count, spec_count):
        for row in block:
            # The gate must see the WHOLE spectrum: the threshold comes from
            # the shape of the noise distribution, and a few hundred windowed
            # rows do not sample it well enough.
            full_db = 10.0 * np.log10(np.where(row > 0.0, row, 1e-300))
            limit = power_dynamic_limit(full_db.astype(np.float32)) if noise_gate else 0.0
            limits[at] = limit

            db = full_db[row_low:row_low + rows].astype(np.float32)
            keep = ~(limit > db)
            gated_db[at] = np.where(keep, db, 0.0)
            window_linear[at] = row[row_low:row_low + rows]
            if archive is not None:
                # SNR as chirpsounder2 defines it. build_spectra has already
                # divided by the row median, so subtracting one is the whole
                # conversion -- see h5_archive's module docstring.
                archive[at] = row[arc_lo:arc_hi] - 1.0
            if snr_db is not None:
                # muf/spectro.py: floor = NOISE_COEF * median(spectrum) taken
                # over the WHOLE spectrum before range gating, then row/floor.
                # build_spectra has already divided by that median, so the
                # coefficient is all that is left to apply.
                rel = window_linear[at] / IONO_SNR_NOISE_COEF
                snr_db[at] = 10.0 * np.log10(
                    np.maximum(rel, IONO_SNR_DB_REF) / IONO_SNR_DB_REF)
            if keep.any():
                block_max = float(window_linear[at][keep].max())
                if block_max > win_max:
                    win_max = block_max
            at += 1
            if progress and at % 64 == 0:
                progress(at, spec_count)
    if at < spec_count:                       # short capture
        gated_db = gated_db[:at]
        if snr_db is not None:
            snr_db = snr_db[:at]
        if archive is not None:
            archive = archive[:at]
        window_linear = window_linear[:at]
        limits = limits[:at]
        spec_count = at

    if noise_gate:
        # Second, statistical gate. After normalisation every spectrum has a
        # median of 1, so the noise level is just the factor.
        noise_db = np.float32(10.0 * math.log10(NOISE_FACTOR))
        alive = gated_db.max(axis=1) > 0.0
        gated_db[alive] = np.where(gated_db[alive] < noise_db, 0.0, gated_db[alive])
        gated_db = delete_small_objects(gated_db, obj_w, obj_h, obj_level)

    luf_index, muf_index = usage_frequencies(gated_db)

    # SNR sums LINEAR normalised power over the points the gate let through.
    snr = np.zeros(spec_count, dtype=np.float32)
    if luf_index >= 0 <= muf_index:
        lo = max(0, luf_index)
        hi = min(spec_count - 1, muf_index)
        for s in range(lo, hi + 1):
            total = window_linear[s][gated_db[s] > 0.0].sum()
            ratio = total / NOISE_FACTOR - 1.0
            if ratio >= 1.0:
                snr[s] = 10.0 * math.log10(ratio)

    # PDP integrates the gated dB array over the spectra inside the band.
    first, last = 0, spec_count - 1
    if luf_index >= 0 <= muf_index:
        first = min(max(0, luf_index), spec_count - 1)
        last = min(max(0, muf_index), spec_count - 1)
    band = gated_db[first:last + 1]
    pdp = np.where(band > 0.0, band, 0.0).sum(axis=0).astype(np.float32)

    span = geo["freq_max_mhz"] - geo["freq_min_mhz"]
    def to_mhz(index):
        if index < 0 or spec_count <= 1:
            return -1.0
        return geo["freq_min_mhz"] + span * index / (spec_count - 1)

    meta = dict(header)
    meta.update(geo)
    meta.update({
        "fft_count": fft_count,
        "spec_count": spec_count,
        "spec_point_count": rows,
        "noise_gate": noise_gate,
        "iono_mode": iono_mode,
        "noise_gate_db": float(np.median(limits)) if noise_gate else 0.0,
        # The console takes its colour scale from these two, so in "snr" mode
        # they carry the fixed 20..75 dB window instead of 0..peak. Scaling to
        # the peak is right for a gated image, where every stored point is
        # trace; it washes out a continuous one, where the peak is the direct
        # signal and the structure worth seeing sits well below it. Values
        # outside the window clamp to the end colours.
        "min_value_db": IONO_SNR_VMIN_DB if iono_mode == IONO_MODE_SNR else 0.0,
        "max_value_db": (IONO_SNR_VMAX_DB if iono_mode == IONO_MODE_SNR
                         else (10.0 * math.log10(win_max) if win_max > 0.0 else 0.0)),
        "luf_index": luf_index, "muf_index": muf_index,
        "luf_mhz": to_mhz(luf_index), "muf_mhz": to_mhz(muf_index),
    })
    sections = {"IONO": gated_db if snr_db is None else snr_db,
                "SNR ": snr.reshape(1, -1),
                "PDP ": pdp.reshape(1, -1)}
    if archive is None:
        return meta, sections

    import h5_archive
    axis, _step = h5_archive.range_axis(header, fft_count)
    return meta, sections, {
        "snr": archive,
        "freqs_hz": h5_archive.freq_axis(header, fft_count, spec_count),
        "ranges_km": axis[arc_lo:arc_hi],
    }


# ---------------------------------------------------------------- writing

def _fixed(text, size):
    raw = text.encode("utf-8")[:size]
    return raw + b"\0" * (size - len(raw))


def write(path, meta, sections, producer="ionozond", producer_version="lfp 1.0"):
    head = bytearray(LFP_HEADER_SIZE)
    head[0:4] = LFP_MAGIC
    struct.pack_into("<HHIIII", head, 4,
                     LFP_VERSION[0], LFP_VERSION[1], LFP_HEADER_SIZE,
                     len(sections), LFP_HEADER_SIZE,
                     1 if meta.get("noise_gate") else 0)
    head[0x18:0x20] = _fixed(producer, 8)
    head[0x20:0x30] = _fixed(producer_version, 16)
    head[0x30:0x70] = _fixed(meta["tx_name"], 64)
    struct.pack_into("<ff", head, 0x70, meta["tx_lat"], meta["tx_lon"])
    head[0x78:0xB8] = _fixed(meta["rx_name"], 64)
    struct.pack_into("<ff", head, 0xB8, meta["rx_lat"], meta["rx_lon"])
    struct.pack_into("<q", head, 0xC0, int(meta["start_epoch"]) * 1000)
    struct.pack_into("<IIII", head, 0xC8, meta["cf"], meta["rate"],
                     meta["sample_rate"], meta["dec"])
    struct.pack_into("<HH", head, 0xD8, meta["dur"], meta["whiten"])
    struct.pack_into("<II", head, 0xDC, meta["whiten_len"], meta["whiten_n"])
    struct.pack_into("<III", head, 0xE4, meta["fft_count"], meta["spec_count"],
                     meta["spec_point_count"])
    struct.pack_into("<ffff", head, 0xF0, meta["freq_min_mhz"], meta["freq_max_mhz"],
                     meta["delay_min_ms"], meta["delay_max_ms"])
    struct.pack_into("<ff", head, 0x100, meta["noise_gate_db"], meta["max_value_db"])
    struct.pack_into("<ff", head, 0x108, meta["luf_mhz"], meta["muf_mhz"])
    struct.pack_into("<ii", head, 0x110, meta["luf_index"], meta["muf_index"])
    struct.pack_into("<II", head, 0x118, meta.get("tb", 0),
                     meta.get("lfsr_polynome_degree", 0))
    # 0x120 onwards was unused. Sidecars written before this read back as
    # min_value_db 0.0 and mode 0, which is exactly the old behaviour.
    struct.pack_into("<fI", head, 0x120, float(meta.get("min_value_db", 0.0)),
                     1 if meta.get("iono_mode") == IONO_MODE_SNR else 0)

    table = bytearray()
    payloads = []
    offset = LFP_HEADER_SIZE + len(sections) * LFP_SECTION_ENTRY
    for name, array in sections.items():
        array = np.ascontiguousarray(array, dtype=np.float32)
        blob = zlib.compress(array.tobytes(), 6)
        entry = struct.pack("<4sHHIIQQ", _fixed(name, 4), 1, 1,
                            array.shape[0], array.shape[1], offset, len(blob))
        table += entry
        payloads.append(blob)
        offset += len(blob)

    tmp = path + ".writing"
    with open(tmp, "wb") as fh:
        fh.write(head)
        fh.write(table)
        for blob in payloads:
            fh.write(blob)
    os.replace(tmp, path)
    return os.path.getsize(path)


def sidecar_path(lfs_path):
    return os.path.splitext(lfs_path)[0] + ".lfp"


def h5_path_for(lfs_path, meta):
    """Where the HDF5 archive goes: beside the capture, but under
    chirpsounder2's name, because muf/loader.py globs for that prefix."""
    import h5_archive
    return os.path.join(os.path.dirname(lfs_path), h5_archive.archive_name(meta))


def build_one(lfs_path, fft_count=16384, force=False, quiet=False,
              h5_archive_km=None, **clean):
    """Build the .lfp sidecar, and optionally the .h5 archive beside it.

    `h5_archive_km` is the (lo, hi) km window the archive keeps, or None for
    no archive. Both come out of one pass over the capture, because the 80 MB
    is the expensive part and reading it twice would double the cost.
    """
    out = sidecar_path(lfs_path)
    fresh = (not force and os.path.exists(out) and
             os.path.getmtime(out) >= os.path.getmtime(lfs_path))
    if fresh and h5_archive_km is None:
        if not quiet:
            print("  %s is up to date" % os.path.basename(out))
        return out, 0

    result = compute(lfs_path, fft_count=fft_count,
                     archive_range_km=h5_archive_km, **clean)
    meta, sections = result[0], result[1]
    size = write(out, meta, sections)

    h5_size = 0
    if h5_archive_km is not None:
        import h5_archive as _h5
        arc = result[2]
        h5_size = _h5.write(h5_path_for(lfs_path, meta), meta, arc["snr"],
                            arc["freqs_hz"], arc["ranges_km"])

    if not quiet:
        raw = os.path.getsize(lfs_path)
        print("  %s  %.1f kB  (%.0fx smaller)  LUF %.2f  MUF %.2f MHz"
              % (os.path.basename(out), size / 1024.0, raw / float(size),
                 meta["luf_mhz"], meta["muf_mhz"]))
        if h5_size:
            print("  %s  %.2f MB  (%.0fx smaller than the capture)"
                  % (os.path.basename(h5_path_for(lfs_path, meta)),
                     h5_size / 1e6, raw / float(h5_size)))
    return out, size


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help=".lfs file, or a directory of them")
    ap.add_argument("--fft", type=int, default=16384)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--obj-w", type=int, default=OBJ_SIZE_HORIZONTAL,
                    help="speckle window across spectra (default %d)" % OBJ_SIZE_HORIZONTAL)
    ap.add_argument("--obj-h", type=int, default=OBJ_SIZE_VERTICAL,
                    help="speckle window across delay rows (default %d)" % OBJ_SIZE_VERTICAL)
    ap.add_argument("--obj-level", type=float, default=OBJ_LEVEL,
                    help="neighbours a point needs to survive; lower keeps more "
                         "of a faint trace and more noise (default %g)" % OBJ_LEVEL)
    ap.add_argument("--iono-mode", choices=[IONO_MODE_GATED, IONO_MODE_SNR],
                    default=IONO_MODE_GATED,
                    help="what the IONO section holds: 'gated' (the original: "
                         "thresholded and despeckled) or 'snr' (chirpsounder2: "
                         "the continuous field, nothing deleted)")
    ap.add_argument("--h5", action="store_true",
                    help="also write a chirpsounder2 lfm_ionogram-*.h5 archive, "
                         "which is what makes deleting the .lfs survivable")
    ap.add_argument("--h5-range-km", default="0,8000",
                    help="delay window the archive keeps, km (default 0,8000 -- "
                         "covers 1F through 3F on a 2634 km path)")
    ap.add_argument("--recurse", action="store_true")
    ap.add_argument("--verify", action="store_true",
                    help="read the sidecar back and print what it holds")
    opts = ap.parse_args()

    targets = []
    if os.path.isdir(opts.target):
        walker = os.walk(opts.target) if opts.recurse else \
            [(opts.target, [], os.listdir(opts.target))]
        for root, _dirs, files in walker:
            targets += [os.path.join(root, f) for f in sorted(files)
                        if f.endswith(".lfs")]
    else:
        targets = [opts.target]

    if not targets:
        print("no .lfs captures under %s" % opts.target)
        return 1

    total_raw = total_side = 0
    h5_window = None
    if opts.h5:
        lo, hi = (float(v) for v in opts.h5_range_km.split(","))
        h5_window = (lo, hi)

    failed = 0
    for path in targets:
        try:
            out, size = build_one(path, fft_count=opts.fft, force=opts.force,
                                  obj_w=opts.obj_w, obj_h=opts.obj_h,
                                  obj_level=opts.obj_level,
                                  iono_mode=opts.iono_mode,
                                  h5_archive_km=h5_window)
            if size:
                total_raw += os.path.getsize(path)
                total_side += size
        except Exception as exc:
            print("  %s: %s" % (os.path.basename(path), exc))
            failed += 1

    if total_side:
        print()
        print("  %d sidecar%s: %.1f MB of captures -> %.1f kB"
              % (len(targets) - failed, "" if len(targets) - failed == 1 else "s",
                 total_raw / 1e6, total_side / 1024.0))

    if opts.verify:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from lfp import read_lfp
        meta, data = read_lfp(sidecar_path(targets[-1]))
        print()
        print("  read back %s:" % os.path.basename(sidecar_path(targets[-1])))
        for key in ("tx_name", "rx_name", "spec_count", "spec_point_count",
                    "freq_min_mhz", "freq_max_mhz", "delay_min_ms",
                    "delay_max_ms", "luf_mhz", "muf_mhz"):
            print("    %-18s %s" % (key, meta[key]))
        for name, arr in sorted(data.items()):
            print("    %-18s %s %s  max %.2f"
                  % (name, arr.shape, arr.dtype, float(arr.max())))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Keep the echo window of a capture's dechirped voltage.

The station deletes each 80 MB capture as soon as its .h5 archive verifies,
and the archive is power: |FFT|^2. That keeps where the echoes are and how
strong, and throws away everything an HF channel is made of on the fast
scale -- each mode's Doppler shift and spread, its fading, the phase that
carries group-delay dispersion, o/x splitting. Until now the sounder threw
that away every five minutes.

Almost none of the 80 MB is echo. After dechirping, an echo at virtual range
r is a tone at a beat proportional to r, and every mode on a circuit sits
within a few hundred km: on cyprus1 -> Yoshkar-Ola, 1F to 3F is a beat band of
a few hundred Hz out of the 40 kHz stored. Shifting that band to zero,
low-passing it and decimating keeps everything the echoes contain, losslessly
within the band, in about 1.6 MB.

Format
------
ionograms-handler's ``docs/echo-window.md``, version 1, is the specification
and ``muf/echo_window.py`` the reference implementation. This module is the
second implementation, for .lfs only, and it is held to the reference rather
than to itself:

- the window is chosen exactly as muf chooses it -- the union of auto_gate on
  the full-axis power ionogram and the 1..3 hop geometry, plus a margin --
  so the same capture gives the same window from either side;
- the cut is summed tap by tap in the order scipy.signal.upfirdn sums, which
  makes its output bit-identical to muf's resample_poly. muf's verifier
  re-cuts with the stored taps and demands identical samples, so this is
  what lets a window written here pass a check run there.

Only numpy and h5py, because scipy is optional on the station. The taps are
designed from the Kaiser formula the specification writes out, and stored,
so no reader ever has to agree with this designer.

The window follows the energy, not the geometry: a 45 ms epoch error once
displaced an echo by 13,500 km, and a window built from the path alone would
have kept an empty band. Interference stays in it: a broadcast carrier
crossing the beat band is what the receiver heard.
"""

import argparse
import json
import math
import os
import shutil
import sys

import numpy as np

GROUP = "echo_window"
FORMAT = "echo_window"
VERSION = 1
SAMPLES = "z"
TAPS = "taps"
ROW_NOISE = "row_noise"
LFS_HEADER = "lfs_header"
SOURCE_LFS = "lfs"
STANDALONE_SUFFIX = ".echo.h5"
LFS_HEADER_SIZE = 512

# muf's constants, repeated rather than imported: the station has no muf. Each
# one feeds the window choice or the cut, so changing any of them here alone
# would make this writer disagree with the reference.
LIGHT_SPEED_KM_S = 3e8 / 1e3
EARTH_RADIUS_KM = 6371.0
HOP_HEIGHTS_KM = (250.0, 300.0, 350.0, 400.0, 450.0, 500.0)
NOISE_COEF = 4 * math.log(2)
NOISE_WINDOW = 8192
DB_FLOOR = 1e-3
DETECTION_DB = 43.0

AUTO_GATE_QUANTILE = 0.98
AUTO_GATE_FLOOR_FRACTION = 0.25
AUTO_GATE_SMOOTH_KM = 40.0

DEFAULT_MARGIN_KM = 150.0
DEFAULT_MAX_HOPS = 3

DOPPLER_MARGIN_HZ = 20.0
TRANSITION_FRACTION = 0.10
MIN_TRANSITION_HZ = 10.0
RATE_HEADROOM = 1.25
STOPBAND_DB = 80.0
MAX_RIPPLE_DB = 0.01
DESIGN_MARGIN_DB = 3.0

CHUNK_SAMPLES = 65536
COMPRESSION_LEVEL = 4

POWER_CHECK_ABOVE_NOISE_DB = 10.0
POWER_CHECK_TOLERANCE_DB = 0.5

CUT_BLOCK = 16384


class NoEchoError(ValueError):
    """Nothing in the capture says where an echo is, so there is no window."""


class VerificationError(RuntimeError):
    """A written window did not verify; nothing was replaced."""


def _h5py():
    try:
        import h5py
    except ImportError:
        raise RuntimeError(
            "h5py is missing:  sudo apt-get install -y python3-h5py")
    return h5py


def _header(path):
    from lfp_products import read_lfs_header
    return read_lfs_header(path)


def source(path):
    """The capture's rates and how its beat frequency maps to range.

    The .lfs axis descends: an echo at range r is a tone at
    -r * div * rate / (2c), with div 2 oblique and 4 vertical. Getting the
    sign wrong keeps an empty band and decimates it perfectly.
    """
    head = _header(path)
    with open(path, "rb") as fh:
        raw = fh.read(LFS_HEADER_SIZE)
    oblique = head["tx_name"] != head["rx_name"]
    div = 2.0 if oblique else 4.0
    return {
        "path": path, "header": head, "raw": raw, "oblique": oblique,
        "sr_hz": head["sample_rate"] / head["dec"],
        "rate_hz_s": float(head["rate"]),
        "t0": float(head["start_epoch"]),
        "sweep_hz_at_0": float(head["cf"] - head["sample_rate"] / 2),
        "km_per_hz": -LIGHT_SPEED_KM_S * 2.0 / (div * head["rate"]),
        "range_zero_km": 0.0,
        "n_samples": (os.path.getsize(path) - LFS_HEADER_SIZE) // 8,
    }


def _samples(path):
    count = (os.path.getsize(path) - LFS_HEADER_SIZE) // 8
    return np.memmap(path, dtype=np.complex64, mode="r",
                     offset=LFS_HEADER_SIZE, shape=(count,))


def _hanning(length):
    n = np.arange(length, dtype=np.float64)
    return 0.5 * (1.0 - np.cos(2.0 * np.pi * n / length))


def _to_db(power):
    return 10.0 * np.log10(np.maximum(power, DB_FLOOR) / DB_FLOOR)


def spectrogram(src):
    """muf's spectro.compute over the whole axis, and its per-row floor.

    Non-overlapping Hann rows of NOISE_WINDOW samples, each divided by
    NOISE_COEF times its own full-band median. The floor is returned too:
    it is the row_noise the window carries, since a narrow window cannot
    recompute a full-band median from itself.
    """
    iq = _samples(src["path"])
    rows = len(iq) // NOISE_WINDOW
    if rows == 0:
        raise ValueError("%s holds %d samples, less than one %d-sample row"
                         % (src["path"], len(iq), NOISE_WINDOW))
    win = _hanning(NOISE_WINDOW)
    padded = np.zeros(NOISE_WINDOW, dtype=np.complex128)
    power = np.empty((rows, NOISE_WINDOW), dtype=np.float32)
    floors = np.empty(rows, dtype=np.float64)
    for i in range(rows):
        padded[:NOISE_WINDOW] = win * iq[i * NOISE_WINDOW:(i + 1) * NOISE_WINDOW]
        spectrum = np.abs(np.fft.fftshift(np.fft.fft(padded))) ** 2
        floor = NOISE_COEF * np.median(spectrum)
        floors[i] = floor
        power[i] = spectrum / floor if floor > 0 else spectrum
    half = LIGHT_SPEED_KM_S * (src["sr_hz"] / (2.0 if src["oblique"] else 4.0)) / src["rate_hz_s"]
    step = 2 * half / NOISE_WINDOW
    return power, half - np.arange(0, NOISE_WINDOW) * step, step, floors


def auto_gate(power, vrange):
    """muf.calibrate.auto_gate with no margin: where the energy stacks up.

    A count of bright cells per range, not a sum of power, so scattered
    noise spikes cannot drag the window open. None when no range stands out.
    """
    power = np.asarray(power)
    vrange = np.asarray(vrange, dtype=np.float64)
    if power.ndim != 2 or power.shape[0] < 2 or len(vrange) < 3:
        return None
    with np.errstate(invalid="ignore"):
        threshold = np.nanquantile(power, AUTO_GATE_QUANTILE, axis=1, keepdims=True)
        profile = (power > threshold).sum(axis=0).astype(np.float64)
    if not np.isfinite(profile).all() or profile.max() <= 0:
        return None
    step = abs(float(vrange[1] - vrange[0])) or 1.0
    width = max(3, int(AUTO_GATE_SMOOTH_KM / step) | 1)
    profile = np.convolve(profile, np.ones(width) / width, mode="same")
    baseline = float(np.median(profile))
    peak = float(profile.max())
    if peak <= baseline * 1.5:
        return None
    cut_level = baseline + AUTO_GATE_FLOOR_FRACTION * (peak - baseline)
    lo_i = hi_i = int(np.argmax(profile))
    while lo_i > 0 and profile[lo_i - 1] > cut_level:
        lo_i -= 1
    while hi_i < len(profile) - 1 and profile[hi_i + 1] > cut_level:
        hi_i += 1
    lo_km, hi_km = sorted((float(vrange[lo_i]), float(vrange[hi_i])))
    return lo_km - 0.0, hi_km + 0.0


def _ground_km(src):
    if not src["oblique"]:
        return None
    head = src["header"]
    lat1, lon1, lat2, lon2 = head["tx_lat"], head["tx_lon"], head["rx_lat"], head["rx_lon"]
    if any(c is None or not math.isfinite(c) for c in (lat1, lon1, lat2, lon2)):
        return None
    if not (-90 <= lat1 <= 90 and -90 <= lat2 <= 90):
        return None
    if not (-180 <= lon1 <= 180 and -180 <= lon2 <= 180):
        return None
    if lat1 == lat2 and lon1 == lon2:
        return None
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_RADIUS_KM * math.asin(min(1.0, math.sqrt(a)))


def _hop_km(ground, hops, height):
    return hops * math.hypot(ground / hops, 2.0 * height)


def geometry_gate(src, max_hops):
    """Where 1..max_hops hops can put an echo. None without a usable path."""
    ground = _ground_km(src)
    if ground is None:
        return None
    lo = _hop_km(ground, 1, min(HOP_HEIGHTS_KM)) - 0.0
    hi = _hop_km(ground, max(1, max_hops), max(HOP_HEIGHTS_KM)) + 0.0
    return max(lo, 0.0), hi


def _edge_db(db, vrange, lo, hi):
    cols = (vrange >= lo) & (vrange <= hi)
    if not cols.any():
        return float("nan")
    return float(np.max(db[:, cols]))


def select(src, power, vrange, step, margin_km=DEFAULT_MARGIN_KM,
           max_hops=DEFAULT_MAX_HOPS, full=False):
    """The range window to keep: the union of both candidates, plus margin.

    With `full`, the whole axis instead, recorded as a manual range: the
    specification's undecimated case, which keeps the capture sample for
    sample when its full voltage is wanted in the archive.
    """
    ends = sorted((src["range_zero_km"] + src["km_per_hz"] * float(-src["sr_hz"] / 2),
                   src["range_zero_km"] + src["km_per_hz"] * float(src["sr_hz"] / 2)))
    auto = geometry = manual = None
    if full:
        manual = (ends[0], ends[1])
        candidates, method, pad = [manual], "manual", 0.0
    else:
        auto = auto_gate(power, vrange)
        geometry = geometry_gate(src, max_hops)
        candidates = [c for c in (auto, geometry) if c is not None]
        if not candidates:
            raise NoEchoError(
                "%s: no range stands out in the power ionogram and its path has "
                "no usable geometry, so there is nothing to keep"
                % os.path.basename(src["path"]))
        method = "+".join(name for name, c in (("auto_gate", auto),
                                               ("geometry", geometry))
                          if c is not None)
        pad = float(margin_km)
    lo = min(c[0] for c in candidates) - pad
    hi = max(c[1] for c in candidates) + pad
    lo, hi = max(lo, ends[0]), min(hi, ends[1])
    if lo >= hi:
        raise NoEchoError("%s: window %.0f..%.0f km lies outside the %.0f..%.0f "
                          "km the samples represent"
                          % (os.path.basename(src["path"]), lo, hi, ends[0], ends[1]))
    db = _to_db(power).astype(np.float32)
    edge = max(float(margin_km), abs(float(step)))
    edges = (_edge_db(db, vrange, lo, lo + edge), _edge_db(db, vrange, hi - edge, hi))

    def pair(v):
        return None if v is None else [round(float(v[0]), 3), round(float(v[1]), 3)]

    return {
        "lo_km": float(lo), "hi_km": float(hi), "method": method,
        "edge_energy_db": edges,
        "truncated_suspect": any(math.isfinite(e) and e >= DETECTION_DB for e in edges),
        "json": json.dumps({"method": method, "margin_km": float(margin_km),
                            "max_hops": int(max_hops), "auto_gate_km": pair(auto),
                            "geometry_km": pair(geometry), "manual_km": pair(manual)},
                           sort_keys=True),
    }


def _kaiser_beta(attenuation):
    if attenuation > 50:
        return 0.1102 * (attenuation - 8.7)
    if attenuation > 21:
        return 0.5842 * (attenuation - 21) ** 0.4 + 0.07886 * (attenuation - 21)
    return 0.0


def _kaiserord(attenuation, width):
    numtaps = (attenuation - 7.95) / 2.285 / (np.pi * width) + 1
    return int(math.ceil(numtaps)), _kaiser_beta(attenuation)


def design(numtaps, beta, cutoff_hz, sr_hz):
    """scipy.signal.firwin(numtaps, cutoff, window=("kaiser", beta), fs, scale=True).

    Written out as the specification gives it: a Kaiser-windowed sinc scaled
    to unit DC gain, so noise power per Hz in the passband is preserved.
    """
    if numtaps <= 1:
        return np.ones(1)
    m = np.arange(0, numtaps) - 0.5 * (numtaps - 1)
    edge = cutoff_hz / (sr_hz / 2.0)
    h = edge * np.sinc(edge * m) * np.kaiser(numtaps, beta)
    return h / np.sum(h)


def _gain(taps, freqs, sr_hz, chunk=256):
    n = np.arange(len(taps), dtype=np.float64)
    out = np.empty(len(freqs))
    for start in range(0, len(freqs), chunk):
        w = 2.0 * np.pi * np.asarray(freqs[start:start + chunk])[:, None] / sr_hz
        re = np.cos(w * n).dot(taps)
        im = np.sin(w * n).dot(taps)
        out[start:start + chunk] = np.hypot(re, im)
    return out


def response(taps, sr_hz, passband, stopband):
    """Measured (stopband attenuation dB, passband ripple dB), on muf's grid."""
    nyq = sr_hz / 2.0
    lobe = 20.0 * sr_hz / max(len(taps), 1)
    stop = np.unique(np.concatenate([
        np.linspace(stopband, min(nyq, stopband + lobe), 4096),
        np.linspace(stopband, nyq, 4096)]))
    with np.errstate(divide="ignore"):
        attenuation = -float(np.max(20 * np.log10(_gain(taps, stop, sr_hz))))
        ripple = float(np.max(np.abs(20 * np.log10(
            _gain(taps, np.linspace(0.0, passband, 2048), sr_hz)))))
    return attenuation, ripple


def plan(src, lo_km, hi_km):
    """The shift, decimation and filter for keeping lo_km..hi_km.

    The kept band is the window's beat span plus a Doppler margin each side;
    the stopband starts inside the output Nyquist, so nothing folds into the
    band at all. The taps are measured and lengthened until they meet the
    requirement, rather than trusted to Kaiser's estimate.
    """
    sr = src["sr_hz"]
    nyq = sr / 2.0
    f1 = (float(lo_km) - src["range_zero_km"]) / src["km_per_hz"]
    f2 = (float(hi_km) - src["range_zero_km"]) / src["km_per_hz"]
    shift = (f1 + f2) / 2.0
    passband = abs(f2 - f1) / 2.0 + DOPPLER_MARGIN_HZ
    transition = max(MIN_TRANSITION_HZ, TRANSITION_FRACTION * 2.0 * passband)
    stopband = passband + transition
    decimation = int(sr // (RATE_HEADROOM * 2.0 * stopband)) if stopband < nyq else 1
    if decimation <= 1:
        return {"source_sr_hz": sr, "shift_hz": shift, "decimation": 1,
                "passband_hz": nyq, "stopband_hz": nyq, "numtaps": 1,
                "beta": 0.0, "cutoff_hz": nyq, "attenuation_db": float("inf"),
                "ripple_db": 0.0, "taps": np.ones(1)}
    cutoff = (passband + stopband) / 2.0
    extra = 0.0
    for _ in range(8):
        numtaps, beta = _kaiserord(STOPBAND_DB + DESIGN_MARGIN_DB + extra, transition / nyq)
        numtaps |= 1
        taps = design(numtaps, beta, cutoff, sr)
        attenuation, ripple = response(taps, sr, passband, stopband)
        if attenuation >= STOPBAND_DB and ripple <= MAX_RIPPLE_DB:
            break
        extra += 3.0
    else:
        raise RuntimeError("filter design did not reach %.0f dB / %.2f dB (got "
                           "%.1f dB, %.4f dB)" % (STOPBAND_DB, MAX_RIPPLE_DB,
                                                   attenuation, ripple))
    return {"source_sr_hz": sr, "shift_hz": shift, "decimation": decimation,
            "passband_hz": passband, "stopband_hz": stopband,
            "numtaps": int(numtaps), "beta": float(beta), "cutoff_hz": cutoff,
            "attenuation_db": attenuation, "ripple_db": ripple, "taps": taps}


def _lead(p):
    """Window samples stored before source sample 0: half the filter, in output samples."""
    if p["numtaps"] <= 1:
        return 0
    return -(-((p["numtaps"] - 1) // 2) // p["decimation"])


def _n_out(p, n):
    if p["numtaps"] <= 1:
        return n
    padded = _lead(p) * p["decimation"] + n + (p["numtaps"] - 1) // 2 + p["decimation"]
    return -(-padded // p["decimation"])


def _mix(x, start, stop, cycles_per_sample):
    """x[m] * exp(j 2 pi f m / sr) over start..stop, with the phase reduced to
    whole cycles first: across ten million samples f m / sr reaches 10^5
    cycles, and the exponential of that spends float64 on the integer part."""
    m = np.arange(start, stop, dtype=np.float64)
    phase = np.mod(cycles_per_sample * m, 1.0)
    return x[start:stop] * np.exp(2j * np.pi * phase)


def cut(samples, p):
    """z[k] = sum_j h[j] y[(k - L) D + (N-1)/2 - j], y the shifted source.

    Blocks of output, so the 80 MB capture is never held widened to
    complex128 at once. Within a block the taps are accumulated one at a
    time in ascending order, which is the order upfirdn sums in: measured on
    cyprus1_20260204_120010, 0 of 222,304 samples differ from muf's
    resample_poly at complex64.
    """
    n = len(samples)
    cycles = float(-p["shift_hz"]) / float(p["source_sr_hz"])
    if p["numtaps"] <= 1:
        out = np.empty(n, dtype=np.complex64)
        for start in range(0, n, CUT_BLOCK * 64):
            stop = min(n, start + CUT_BLOCK * 64)
            out[start:stop] = _mix(samples, start, stop, cycles).astype(np.complex64)
        return out
    h = np.asarray(p["taps"], dtype=np.float64)
    taps, d = len(h), p["decimation"]
    half = (taps - 1) // 2
    shift = _lead(p) * d
    total = _n_out(p, n)
    out = np.empty(total, dtype=np.complex64)
    for k0 in range(0, total, CUT_BLOCK):
        k1 = min(total, k0 + CUT_BLOCK)
        m_lo = k0 * d + half - (taps - 1) - shift
        m_hi = (k1 - 1) * d + half - shift
        seg = np.zeros(m_hi - m_lo + 1, dtype=np.complex128)
        a, b = max(m_lo, 0), min(m_hi, n - 1)
        if a <= b:
            seg[a - m_lo:b - m_lo + 1] = _mix(samples, a, b + 1, cycles)
        acc = np.zeros(k1 - k0, dtype=np.complex128)
        span = (k1 - k0 - 1) * d + 1
        for j in range(taps):
            first = taps - 1 - j
            acc += h[j] * seg[first:first + span:d]
        out[k0:k1] = acc.astype(np.complex64)
    return out


def reconstruct(window):
    """The window back at the source rate: interpolated with the stored taps
    and shifted back. Alias-free by design, so this is the source passed
    through the filter twice."""
    h = np.asarray(window["taps"], dtype=np.float64)
    d = window["decimation"]
    z = np.asarray(window["z"], dtype=np.complex128)
    n_src = window["source_samples"]
    if len(h) <= 1:
        up = z
    else:
        stuffed = np.zeros(len(z) * d, dtype=np.complex128)
        stuffed[::d] = z
        size = len(stuffed) + len(h)
        up = d * np.fft.ifft(np.fft.fft(stuffed, size) * np.fft.fft(h, size))
        up = up[(len(h) - 1) // 2:][:len(stuffed)]
    start = int(round(-window["t_first_s"] * window["source_sr_hz"]))
    up = up[start:start + n_src]
    if len(up) < n_src:
        up = np.concatenate([up, np.zeros(n_src - len(up), up.dtype)])
    return _mix(up, 0, n_src, float(window["shift_hz"]) / float(window["source_sr_hz"]))


def _filter_json(p):
    return json.dumps({
        "type": "FIR, Kaiser window, linear phase, applied zero-phase "
                "polyphase (numpy, summed in scipy.signal.upfirdn's order)",
        "numtaps": p["numtaps"],
        "kaiser_beta": round(p["beta"], 6),
        "cutoff_hz": p["cutoff_hz"],
        "passband_edge_hz": p["passband_hz"],
        "stopband_edge_hz": p["stopband_hz"],
        "stopband_attenuation_db": round(p["attenuation_db"], 3),
        "passband_ripple_db": round(p["ripple_db"], 6),
        "gain": "unit DC gain (sum of taps = 1): noise power per Hz preserved",
        "doppler_margin_hz": DOPPLER_MARGIN_HZ,
    }, sort_keys=True)


def _writer():
    from h5_archive import _git_metadata
    commit, dirty = _git_metadata()
    if commit is None:
        return "ionozond unknown"
    return "ionozond %s%s" % (commit, "-dirty" if dirty else "")


def _attrs(src, sel, p):
    t_first = -_lead(p) * p["decimation"] / src["sr_hz"]
    return {
        "format": FORMAT,
        "version": np.int64(VERSION),
        "source": SOURCE_LFS,
        "source_name": os.path.basename(src["path"]),
        "source_samples": np.int64(src["n_samples"]),
        "source_sr_hz": float(src["sr_hz"]),
        "sr_hz": float(p["source_sr_hz"] / p["decimation"]),
        "decimation": np.int64(p["decimation"]),
        "shift_hz": float(p["shift_hz"]),
        "range_lo_km": float(sel["lo_km"]),
        "range_hi_km": float(sel["hi_km"]),
        "range_axis": "absolute",
        "t0_unix_s": float(src["t0"]),
        "t_first_s": t_first,
        "sweep_hz_at_t_first": float(src["sweep_hz_at_0"] + src["rate_hz_s"] * t_first),
        "rate_hz_s": float(src["rate_hz_s"]),
        "km_per_hz": float(src["km_per_hz"]),
        "range_zero_km": float(src["range_zero_km"]),
        "source_window": np.int64(NOISE_WINDOW),
        "source_step": np.int64(NOISE_WINDOW),
        "filter": _filter_json(p),
        "selection": sel["json"],
        "edge_energy_db": np.asarray(sel["edge_energy_db"], dtype=np.float64),
        "truncated_suspect": np.bool_(sel["truncated_suspect"]),
        "writer": _writer(),
    }


def _standalone_attrs(src):
    head = src["header"]
    attrs = {
        "source_format": "lfs",
        "source_name": os.path.basename(src["path"]),
        "rate_hz_s": float(head["rate"]),
        "sample_rate_hz": float(head["sample_rate"]),
        "dec": np.int64(head["dec"]),
        "sr_hz": float(src["sr_hz"]),
        "t0_unix_s": float(src["t0"]),
        "sweep_start_hz": float(src["sweep_hz_at_0"]),
        "tx_name": head["tx_name"],
        "rx_name": head["rx_name"],
    }
    coords = (head["tx_lat"], head["tx_lon"], head["rx_lat"], head["rx_lon"])
    if all(c is not None and math.isfinite(c) for c in coords):
        attrs.update(tx_latitude=float(coords[0]), tx_longitude=float(coords[1]),
                     rx_latitude=float(coords[2]), rx_longitude=float(coords[3]))
    return attrs


def _write_group(fh, z, attrs, taps, row_noise, raw_header):
    group = fh.create_group(GROUP)
    n = len(z)
    chunk = max(1, -(-n // max(1, -(-n // CHUNK_SAMPLES))))
    group.create_dataset(SAMPLES, data=np.asarray(z, dtype=np.complex64),
                         chunks=(chunk,), compression="gzip",
                         compression_opts=COMPRESSION_LEVEL, shuffle=True)
    for key, value in attrs.items():
        group.attrs[key] = value
    group.create_dataset(TAPS, data=np.asarray(taps, dtype=np.float64))
    noise = group.create_dataset(ROW_NOISE, data=np.asarray(row_noise, dtype=np.float64))
    noise.attrs["window"] = np.int64(NOISE_WINDOW)
    group.create_dataset(LFS_HEADER, data=np.frombuffer(bytes(raw_header), dtype=np.uint8))


def _text(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, np.ndarray) and value.ndim == 0:
        return _text(value[()])
    return str(value)


def has_window(path):
    h5py = _h5py()
    try:
        with h5py.File(path, "r") as fh:
            return GROUP in fh
    except OSError:
        return False


def read(path):
    """The echo_window group of `path`: samples, taps and every attribute."""
    h5py = _h5py()
    with h5py.File(path, "r") as fh:
        if GROUP not in fh:
            raise ValueError("%s: no %r group" % (path, GROUP))
        g = fh[GROUP]
        a = g.attrs
        if _text(a.get("format", "")) != FORMAT:
            raise ValueError("%s: %r is not format %r" % (path, GROUP, FORMAT))
        if int(a["version"]) != VERSION:
            raise ValueError("%s: echo window version %d; this reader understands %d"
                             % (path, int(a["version"]), VERSION))
        if TAPS not in g:
            raise ValueError("%s: echo window has no %r" % (path, TAPS))
        window = {}
        for key in a:
            value = a[key]
            window[key] = _text(value) if isinstance(value, (bytes, str)) else value
        window["filter"] = json.loads(_text(a["filter"]))
        window["selection"] = json.loads(_text(a["selection"]))
        for key in ("source_samples", "decimation", "source_window", "source_step", "version"):
            window[key] = int(a[key])
        for key in ("source_sr_hz", "sr_hz", "shift_hz", "range_lo_km", "range_hi_km",
                    "t0_unix_s", "t_first_s", "sweep_hz_at_t_first", "rate_hz_s",
                    "km_per_hz", "range_zero_km"):
            window[key] = float(a[key])
        window["truncated_suspect"] = bool(a["truncated_suspect"])
        window["z"] = np.asarray(g[SAMPLES][()], dtype=np.complex64)
        window["taps"] = np.asarray(g[TAPS][()], dtype=np.float64)
        if window["taps"].shape != (int(window["filter"]["numtaps"]),):
            raise ValueError("%s: %d stored taps, filter records %d"
                             % (path, len(window["taps"]), window["filter"]["numtaps"]))
        if ROW_NOISE in g:
            window["row_noise"] = np.asarray(g[ROW_NOISE][()], dtype=np.float64)
        if LFS_HEADER in g:
            window["lfs_header"] = bytes(np.asarray(g[LFS_HEADER][()], dtype=np.uint8))
    return window


def is_sound(path, samples=None):
    """Does the window read back, whole? Returns (ok, why).

    What the sounder checks before deleting a capture: not that the write
    returned, but that the bytes reached the disk in a state another program
    can read. With `samples`, the stored z must equal them exactly.
    """
    try:
        window = read(path)
    except Exception as exc:
        return False, "unreadable: %s" % exc
    if len(window["z"]) == 0 or not np.isfinite(window["z"]).all():
        return False, "z is empty or not finite"
    if samples is not None and not np.array_equal(window["z"], samples):
        return False, "z differs from what was cut"
    return True, ""


def verify(path, lfs):
    """Check a stored window against its capture. Returns (ok, problems, metrics).

    The stored taps meet the filter requirement, measured afresh; re-cutting
    the capture with them gives the stored samples exactly; and the power
    re-derived from the window matches the capture's own inside the window.
    """
    problems, metrics = [], {}
    try:
        window = read(path)
    except Exception as exc:
        return False, ["unreadable: %s" % exc], metrics
    p = dict(source_sr_hz=window["source_sr_hz"], shift_hz=window["shift_hz"],
             decimation=window["decimation"], numtaps=len(window["taps"]),
             taps=window["taps"])
    if p["numtaps"] > 1:
        stop, ripple = response(window["taps"], window["source_sr_hz"],
                                window["filter"]["passband_edge_hz"],
                                window["filter"]["stopband_edge_hz"])
        metrics["filter"] = "%.1f dB / %.4f dB" % (stop, ripple)
        if stop < STOPBAND_DB or ripple > MAX_RIPPLE_DB:
            problems.append("stored filter gives %.1f dB stopband, %.4f dB ripple"
                            % (stop, ripple))
    samples = _samples(lfs)
    if len(samples) != window["source_samples"]:
        problems.append("capture has %d samples, window records %d"
                        % (len(samples), window["source_samples"]))
    again = cut(samples, p)
    if not np.array_equal(again, window["z"]):
        problems.append("re-cut differs from the stored samples")
    else:
        metrics["re-cut"] = "identical"
    rec = reconstruct(window)
    win = _hanning(NOISE_WINDOW)
    beats = np.fft.fftfreq(NOISE_WINDOW, d=1.0 / window["source_sr_hz"])
    ranges = window["range_zero_km"] + window["km_per_hz"] * beats
    inside = (ranges >= window["range_lo_km"]) & (ranges <= window["range_hi_km"])
    full, derived = [], []
    for i in range(min(len(samples), len(rec)) // NOISE_WINDOW):
        seg = slice(i * NOISE_WINDOW, (i + 1) * NOISE_WINDOW)
        p_full = np.abs(np.fft.fft(win * samples[seg])) ** 2
        p_rec = np.abs(np.fft.fft(win * rec[seg])) ** 2
        floor = NOISE_COEF * float(np.median(p_full))
        if floor > 0:
            full.append(_to_db(p_full[inside] / floor))
            derived.append(_to_db(p_rec[inside] / floor))
    full = np.concatenate(full) if full else np.zeros(0)
    derived = np.concatenate(derived) if derived else np.zeros(0)
    above = full >= _to_db(1.0 / NOISE_COEF) + POWER_CHECK_ABOVE_NOISE_DB
    if above.any():
        delta = float(np.median(np.abs(full[above] - derived[above])))
        metrics["power"] = "%.3f dB median over %d cells" % (delta, int(above.sum()))
        if delta > POWER_CHECK_TOLERANCE_DB:
            problems.append("window power differs by %.2f dB median" % delta)
    else:
        metrics["power"] = "no cell 10 dB above noise to compare"
    return not problems, problems, metrics


def target_for(lfs, into=None, out_dir=None):
    if into is not None:
        return into
    stem = os.path.splitext(os.path.basename(lfs))[0]
    return os.path.join(out_dir or os.path.dirname(os.path.abspath(lfs)),
                        stem + STANDALONE_SUFFIX)


def write(lfs, into=None, out_dir=None, margin_km=DEFAULT_MARGIN_KM,
          max_hops=DEFAULT_MAX_HOPS, check=False, dry_run=False, full=False):
    """Cut and store the echo window of one capture. Idempotent.

    Into `into`, a chirpsounder2-format archive of the same capture, or a
    standalone <stem>.echo.h5 in `out_dir` (default: beside the capture).
    The capture is only ever read. Every write goes to a temporary sibling,
    is read back, and only then renamed into place, so an interruption
    leaves the target exactly as it was. With `full`, the window is the
    whole band, undecimated: the capture's full voltage, about 69 MB. Returns
    a report dict.
    """
    h5py = _h5py()
    target = target_for(lfs, into, out_dir)
    if into is not None and not os.path.exists(into):
        raise FileNotFoundError("%s: --into must name an existing archive" % into)
    if os.path.exists(target) and has_window(target):
        report = {"status": "present", "target": target}
        if check:
            report["verification"] = verify(target, lfs)
        return report
    src = source(lfs)
    power, vrange, step, floors = spectrogram(src)
    sel = select(src, power, vrange, step, margin_km, max_hops, full)
    del power
    p = plan(src, sel["lo_km"], sel["hi_km"])
    report = {"status": "dry-run" if dry_run else "written", "target": target,
              "lo_km": sel["lo_km"], "hi_km": sel["hi_km"], "method": sel["method"],
              "decimation": p["decimation"], "sr_hz": p["source_sr_hz"] / p["decimation"],
              "numtaps": p["numtaps"], "truncated_suspect": sel["truncated_suspect"],
              "bytes": _n_out(p, src["n_samples"]) * 8}
    if dry_run:
        return report
    z = cut(_samples(lfs), p)
    folder = os.path.dirname(os.path.abspath(target))
    if not os.path.isdir(folder):
        os.makedirs(folder)
    tmp = os.path.join(folder, ".%s.%d.tmp" % (os.path.basename(target), os.getpid()))
    try:
        if os.path.exists(target):
            shutil.copyfile(target, tmp)
        with h5py.File(tmp, "a" if os.path.exists(target) else "w") as fh:
            if not os.path.exists(target):
                for key, value in _standalone_attrs(src).items():
                    fh.attrs[key] = value
            _write_group(fh, z, _attrs(src, sel, p), p["taps"], floors, src["raw"])
        sound, why = is_sound(tmp, z)
        if not sound:
            raise VerificationError("%s: the written window does not read back (%s)"
                                    % (os.path.basename(target), why))
        if check:
            report["verification"] = verify(tmp, lfs)
            if not report["verification"][0]:
                raise VerificationError("%s: %s" % (os.path.basename(target),
                                                    "; ".join(report["verification"][1])))
        os.replace(tmp, target)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    report["file_bytes"] = os.path.getsize(target)
    return report


def _line(lfs, report):
    bits = ["%s: %s" % (os.path.basename(lfs), report["status"])]
    if "lo_km" in report:
        bits.append("%.0f-%.0f km [%s], %.1f Hz (/%d), %.2f MB"
                    % (report["lo_km"], report["hi_km"], report["method"],
                       report["sr_hz"], report["decimation"], report["bytes"] / 1e6))
        if report["truncated_suspect"]:
            bits.append("TRUNCATION SUSPECTED")
    if "verification" in report:
        ok, problems, metrics = report["verification"]
        bits.append("verified (%s)" % ", ".join("%s %s" % kv for kv in sorted(metrics.items()))
                    if ok else "VERIFICATION FAILED: " + "; ".join(problems))
    bits.append("-> %s" % report["target"])
    return " -- ".join(bits)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="+", help=".lfs captures, or directories of them")
    ap.add_argument("--margin-km", type=float, default=DEFAULT_MARGIN_KM,
                    help="range kept beyond the echoes on each side (default %g)"
                    % DEFAULT_MARGIN_KM)
    ap.add_argument("--max-hops", type=int, default=DEFAULT_MAX_HOPS,
                    help="hops the geometry candidate covers (default %d)" % DEFAULT_MAX_HOPS)
    ap.add_argument("--into", help="append to this chirpsounder2-format archive "
                    "of the same capture instead of writing <stem>.echo.h5")
    ap.add_argument("--out", help="write <stem>.echo.h5 here (default: beside the capture)")
    ap.add_argument("--full", action="store_true",
                    help="keep the whole band, undecimated: the full voltage, about 69 MB")
    ap.add_argument("--verify", action="store_true", help="re-cut and compare after writing")
    ap.add_argument("--dry-run", action="store_true", help="print the window and size, write nothing")
    args = ap.parse_args(argv)
    paths = []
    for t in args.target:
        if os.path.isdir(t):
            paths.extend(sorted(os.path.join(t, n) for n in os.listdir(t) if n.endswith(".lfs")))
        else:
            paths.append(t)
    if args.into is not None and len(paths) != 1:
        ap.error("--into takes exactly one capture")
    failed = 0
    for lfs in paths:
        try:
            print(_line(lfs, write(lfs, into=args.into, out_dir=args.out,
                                   margin_km=args.margin_km, max_hops=args.max_hops,
                                   check=args.verify, dry_run=args.dry_run,
                                   full=args.full)))
        except NoEchoError as exc:
            print("%s: skipped -- %s" % (os.path.basename(lfs), exc))
        except Exception as exc:
            failed += 1
            print("%s: FAILED -- %s" % (os.path.basename(lfs), exc))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

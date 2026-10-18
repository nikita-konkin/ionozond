import os

import numpy as np
import pytest

import echo_window
import lfp_products


def beat_at_peak(window, pad=16):
    z = window["z"]
    spectrum = np.abs(np.fft.fft(z, len(z) * pad))
    return np.fft.fftfreq(len(z) * pad, 1.0 / window["sr_hz"])[np.argmax(spectrum)] + window["shift_hz"]


def standalone(capture, tmp_path, **kwargs):
    lfs = capture(**kwargs)
    report = echo_window.write(lfs, out_dir=str(tmp_path / "echo"))
    return lfs, report["target"]


def test_echo_sits_at_its_injected_range(capture, tmp_path):
    distance = np.random.uniform(2600.0, 3400.0)
    _, path = standalone(capture, tmp_path, echoes=((distance, 30.0, 0.0),))
    window = echo_window.read(path)
    found = window["range_zero_km"] + window["km_per_hz"] * beat_at_peak(window)
    assert abs(found - distance) < 1.0, "echo at %.1f km did not come back at its range, %.1f km" % (distance, found)


def test_doppler_shift_survives_the_cut(capture, tmp_path):
    doppler = np.random.uniform(-1.5, 1.5)
    _, path = standalone(capture, tmp_path, echoes=((2750.0, 30.0, doppler),))
    window = echo_window.read(path)
    found = beat_at_peak(window) - 2750.0 / window["km_per_hz"]
    assert abs(found - doppler) < 0.02, "Doppler %.3f Hz did not survive, read %.3f Hz" % (doppler, found)


def test_tone_outside_the_window_is_cut_by_the_stopband(capture):
    lfs = capture()
    source = echo_window.source(lfs)
    plan = echo_window.plan(source, 2500.0, 4100.0)
    tone = np.exp(2j * np.pi * (plan["shift_hz"] + np.random.uniform(3000.0, 9000.0)) * np.arange(200000) / source["sr_hz"])
    out = echo_window.cut(tone.astype(np.complex64), plan)
    level = 20.0 * np.log10(np.sqrt(np.mean(np.abs(out[100:-100]) ** 2)))
    assert level <= -80.0, "an out-of-band tone leaked through at %.1f dB" % level


def test_filter_reaches_eighty_db_of_stopband(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    window = echo_window.read(path)
    stop, _ = echo_window.response(window["taps"], window["source_sr_hz"], window["filter"]["passband_edge_hz"], window["filter"]["stopband_edge_hz"])
    assert stop >= 80.0, "stopband reached only %.1f dB" % stop


def test_filter_ripple_stays_under_a_hundredth_of_a_db(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    window = echo_window.read(path)
    _, ripple = echo_window.response(window["taps"], window["source_sr_hz"], window["filter"]["passband_edge_hz"], window["filter"]["stopband_edge_hz"])
    assert ripple <= 0.01, "passband ripple reached %.4f dB" % ripple


def test_round_trip_equals_the_source_filtered_twice(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path, echoes=((2700.0, 30.0, 0.3), (3300.0, 10.0, -0.7)))
    window = echo_window.read(path)
    x = np.fromfile(lfs, dtype=np.complex64, offset=512).astype(np.complex128)
    m = np.arange(len(x))
    y = x * np.exp(-2j * np.pi * window["shift_hz"] * m / window["source_sr_hz"])
    hh = np.convolve(window["taps"], window["taps"])
    size = len(y) + len(hh)
    twice = np.fft.ifft(np.fft.fft(y, size) * np.fft.fft(hh, size))[len(window["taps"]) - 1:][:len(y)]
    expected = twice * np.exp(2j * np.pi * window["shift_hz"] * m / window["source_sr_hz"])
    error = np.sqrt(np.mean(np.abs(echo_window.reconstruct(window) - expected) ** 2) / np.mean(np.abs(expected) ** 2))
    assert error < 1e-3, "round trip missed the twice-filtered source by %.2e" % error


def test_first_sample_is_the_filter_lead_before_the_capture(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    window = echo_window.read(path)
    lead = -(-((window["filter"]["numtaps"] - 1) // 2) // window["decimation"])
    assert window["t_first_s"] == -lead * window["decimation"] / window["source_sr_hz"], "t_first_s %r is not the filter lead" % window["t_first_s"]


def test_sweep_at_the_first_sample_follows_the_header(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    window = echo_window.read(path)
    expected = 12500000 - 25000000 / 2.0 + 100000 * window["t_first_s"]
    assert window["sweep_hz_at_t_first"] == pytest.approx(expected, abs=1e-6), "sweep at the first sample %r is not the header's" % window["sweep_hz_at_t_first"]


def test_row_noise_is_the_full_band_median(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    x = np.fromfile(lfs, dtype=np.complex64, offset=512, count=8192)
    hann = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(8192) / 8192))
    expected = 4.0 * np.log(2.0) * np.median(np.abs(np.fft.fft(hann * x)) ** 2)
    assert echo_window.read(path)["row_noise"][0] == pytest.approx(expected, rel=1e-12), "row_noise is not the full-band median of the first row"


def test_standalone_window_carries_the_capture_header(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    with open(lfs, "rb") as fh:
        head = fh.read(512)
    assert echo_window.read(path)["lfs_header"] == head, "the standalone window lost the capture header"


def test_window_appended_to_the_archive_leaves_its_ionogram_alone(capture):
    import h5py
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    with h5py.File(archive, "r") as fh:
        before = fh["SNR"][()]
    echo_window.write(lfs, into=archive)
    with h5py.File(archive, "r") as fh:
        after = fh["SNR"][()]
    assert np.array_equal(before, after, equal_nan=True), "appending the window changed the archive's SNR"


def test_second_write_leaves_the_file_byte_identical(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    with open(path, "rb") as fh:
        first = fh.read()
    echo_window.write(lfs, out_dir=str(tmp_path / "echo"))
    with open(path, "rb") as fh:
        second = fh.read()
    assert first == second, "a second write changed a window that was already there"


def test_interrupted_write_leaves_the_archive_intact(capture, monkeypatch):
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    with open(archive, "rb") as fh:
        before = fh.read()

    def broken(*args, **kwargs):
        raise OSError("disk pulled mid-write")

    monkeypatch.setattr(echo_window, "_write_group", broken)
    with pytest.raises(OSError):
        echo_window.write(lfs, into=archive)
    with open(archive, "rb") as fh:
        after = fh.read()
    assert before == after, "an interrupted write damaged the archive"


def test_interrupted_write_leaves_no_temporary_file(capture, monkeypatch):
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))

    def broken(*args, **kwargs):
        raise OSError("disk pulled mid-write")

    monkeypatch.setattr(echo_window, "_write_group", broken)
    with pytest.raises(OSError):
        echo_window.write(lfs, into=archive)
    assert [n for n in os.listdir(os.path.dirname(archive)) if n.endswith(".tmp")] == [], "an interrupted write left its temporary file behind"


def test_silent_vertical_sounding_has_no_window(capture, tmp_path):
    lfs = capture(echoes=(), noise=0.0, tx="yoshkar-ola", rx="yoshkar-ola")
    with pytest.raises(echo_window.NoEchoError):
        echo_window.write(lfs, out_dir=str(tmp_path))


def test_written_window_is_sound(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    assert echo_window.is_sound(path)[0], "a freshly written window does not read back"


def test_window_without_taps_is_not_sound(capture, tmp_path):
    import h5py
    _, path = standalone(capture, tmp_path)
    with h5py.File(path, "a") as fh:
        del fh["echo_window/taps"]
    assert not echo_window.is_sound(path)[0], "a window without its taps passed as sound"


def test_fresh_window_verifies_against_its_capture(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    ok, problems, _ = echo_window.verify(path, lfs)
    assert ok, "a fresh window failed verification: %s" % "; ".join(problems)


def test_window_from_another_capture_fails_verification(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    other = capture(name="cyprus1_20260204_120510.lfs")
    assert not echo_window.verify(path, other)[0], "a window verified against a capture it was not cut from"


def test_writer_names_ionozond(capture, tmp_path):
    _, path = standalone(capture, tmp_path)
    assert echo_window.read(path)["writer"].startswith("ionozond "), "the window does not say ionozond wrote it"


def test_full_voltage_keeps_every_sample_of_the_capture(capture, tmp_path):
    lfs = capture(echoes=((np.random.uniform(2600.0, 3400.0), 20.0, 0.4),))
    target = echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True)["target"]
    source = np.fromfile(lfs, dtype=np.complex64, offset=512)
    assert np.array_equal(echo_window.read(target)["z"], source), "the full-voltage window is not the capture sample for sample"


def test_full_voltage_is_not_decimated(capture, tmp_path):
    lfs = capture()
    target = echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True)["target"]
    assert echo_window.read(target)["decimation"] == 1, "the full-voltage window was decimated"


def test_full_voltage_covers_the_whole_axis(capture, tmp_path):
    lfs = capture()
    window = echo_window.read(echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True)["target"])
    assert (window["range_lo_km"], window["range_hi_km"]) == (-60000.0, 60000.0), "the full-voltage window does not span the whole delay axis"


def test_full_voltage_verifies_against_its_capture(capture, tmp_path):
    lfs = capture()
    target = echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True)["target"]
    ok, problems, _ = echo_window.verify(target, lfs)
    assert ok, "a full-voltage window failed verification: %s" % "; ".join(problems)


# Selection rule 2: what the October 2026 soundings taught. The same cases as
# muf's tests/test_echo_window.py, so the two writers are held to one rule.

def tilted_noise(tilt_db=1.5, seed=0):
    """Noise whose power rises tilt_db across the band, as the receiver's
    response tilted it on all five October 2026 circuits."""
    def make(n, sr):
        rng = np.random.default_rng(seed)
        z = (rng.standard_normal(n) + 1j * rng.standard_normal(n)) / np.sqrt(2.0)
        f = np.fft.fftfreq(n, 1.0 / sr)
        return np.fft.ifft(np.fft.fft(z) * 10 ** (tilt_db * (f / sr) / 20))
    return make


def carriers(rows, lo_hz=-6000.0, hi_hz=4000.0, amplitude=10.0):
    """A broadcast carrier crossing the sweep in each of `rows`: dechirped,
    it runs through a quarter of the beat band during one spectrogram row."""
    def make(n, sr):
        x = np.zeros(n, dtype=np.complex128)
        w = echo_window.NOISE_WINDOW
        t = np.arange(w) / sr
        k = (hi_hz - lo_hz) / (w / sr)
        for i in rows:
            x[i * w:(i + 1) * w] = amplitude * np.exp(2j * np.pi * (lo_hz * t + k * t * t / 2))
        return x
    return make


def scatter(range_km, sigma_hz=40.0, power=200.0, seed=5):
    """A spread echo with an exactly Gaussian spectrum about range_km."""
    def make(n, sr):
        rng = np.random.default_rng(seed)
        f = np.fft.fftfreq(n, 1.0 / sr)
        mag = np.sqrt(np.exp(-0.5 * ((f - range_km / -3.0) / sigma_hz) ** 2))
        x = np.fft.ifft(mag * np.exp(2j * np.pi * rng.random(n)))
        return x * np.sqrt(power / np.mean(np.abs(x) ** 2))
    return make


def test_a_silent_band_keeps_the_geometry_alone(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=(), noise=0.0, extra=tilted_noise())
    window = echo_window.read(path)
    selection = window["selection"]
    assert selection["method"] == "geometry", "a silent band found an echo: %r" % selection
    assert selection["rule"] == echo_window.SELECTION_RULE
    assert abs(window["range_lo_km"] - (selection["geometry_km"][0] - 150.0)) < 1e-3
    assert abs(window["range_hi_km"] - (selection["geometry_km"][1] + 150.0)) < 1e-3
    assert window["decimation"] > 10, "a silent band was kept at /%d" % window["decimation"]
    assert window["trace_at_edge"] is False


def test_a_signal_far_from_the_path_stays_out_beside_its_echo(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=((2900.0, 30.0, 0.0), (-40000.0, 30.0, 0.0)))
    window = echo_window.read(path)
    assert window["selection"]["echo_gate_reach"] == "near"
    assert 0.0 < window["range_lo_km"] < 2900.0 < window["range_hi_km"], "window %.0f-%.0f km" % (
        window["range_lo_km"], window["range_hi_km"])
    assert window["decimation"] > 10


def test_scatter_beyond_the_geometry_comes_with_the_echo(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=((2900.0, 30.0, 0.0),), extra=scatter(4600.0))
    window = echo_window.read(path)
    assert window["selection"]["geometry_km"][1] < 4400.0
    assert window["selection"]["echo_gate_runs"] == 2
    assert window["range_hi_km"] > 4700.0, "scatter at 4600 km was cut at %.0f km" % window["range_hi_km"]


def test_a_carrier_at_the_edge_is_not_a_trace(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=((2900.0, 30.0, 0.0),),
                         extra=carriers(rows=(5, 17, 29, 41)))
    window = echo_window.read(path)
    assert window["truncated_suspect"], "rule 1's flag should still see the carrier"
    assert window["trace_at_edge"] is False, "a carrier was taken for a trace at the edge"


def test_the_far_echo_is_still_kept_when_the_path_has_none(capture, tmp_path):
    """The 45 ms epoch error: nothing near the geometry, an echo far off."""
    _, path = standalone(capture, tmp_path, echoes=((16400.0, 30.0, 0.0),))
    window = echo_window.read(path)
    assert window["selection"]["echo_gate_reach"] == "far"
    assert window["range_lo_km"] < 16400.0 < window["range_hi_km"]


def _make_rule_one(path):
    import json
    h5py = echo_window._h5py()
    with h5py.File(path, "a") as fh:
        attrs = fh[echo_window.GROUP].attrs
        selection = json.loads(echo_window._text(attrs["selection"]))
        selection.pop("rule")
        attrs["selection"] = json.dumps(selection, sort_keys=True)
        del attrs["trace_at_edge"]
        del attrs["edge_sigma"]


def test_recut_replaces_a_rule_one_window_and_nothing_else(capture):
    import h5py
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    with h5py.File(archive, "r") as fh:
        before = {k: fh[k][()] for k in fh if k != echo_window.GROUP and isinstance(fh[k], h5py.Dataset)}
    echo_window.write(lfs, into=archive)
    _make_rule_one(archive)
    assert echo_window.window_rule(archive) == 1
    assert echo_window.read(archive)["trace_at_edge"] is None
    size = os.path.getsize(archive)
    report = echo_window.write(lfs, into=archive, recut=True, check=True)
    assert report["status"] == "replaced"
    assert report["verification"][0], report["verification"][1]
    assert echo_window.window_rule(archive) == echo_window.SELECTION_RULE
    assert os.path.getsize(archive) <= size * 1.01, "the replaced window left its old bytes behind"
    with h5py.File(archive, "r") as fh:
        for key, value in before.items():
            assert np.array_equal(fh[key][()], value), "%s changed when the window was cut again" % key


def test_recut_leaves_a_rule_two_window_alone(capture, tmp_path):
    lfs, path = standalone(capture, tmp_path)
    digest = open(path, "rb").read()
    report = echo_window.write(lfs, out_dir=str(tmp_path / "echo"), recut=True)
    assert report["status"] == "present"
    assert open(path, "rb").read() == digest


def test_recut_leaves_a_full_voltage_window_alone(capture, tmp_path):
    """A manual range is the same window under any rule."""
    lfs = capture()
    path = echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True)["target"]
    _make_rule_one(path)
    assert not echo_window.outdated(path)
    assert echo_window.write(lfs, out_dir=str(tmp_path / "echo"), full=True,
                             recut=True)["status"] == "present"

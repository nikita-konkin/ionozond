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


def gate(lfs):
    power, vrange, _, _ = echo_window.spectrogram(echo_window.source(lfs))
    return echo_window.auto_gate(power, vrange)


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


def test_noise_alone_opens_no_gate(capture):
    assert gate(capture(echoes=(), seconds=60)) is None, "noise alone opened a gate"


def test_floor_rising_toward_the_band_edges_opens_no_gate(capture):
    assert gate(capture(echoes=(), seconds=60, edge=np.random.uniform(0.8, 1.2))) is None, "a floor rising toward the band edges opened a gate"


def test_short_echo_beside_a_rising_band_edge_draws_the_gate(capture):
    distance = np.random.uniform(1000.0, 2000.0)
    found = gate(capture(echoes=((distance, np.random.uniform(0.2, 0.4), 0.0, np.random.uniform(2.5, 4.0)),), seconds=60, edge=np.random.uniform(0.8, 1.2)))
    assert found is not None and found[0] <= distance <= found[1], "a short echo at %.0f km did not draw the gate, which went to %s" % (distance, found)


def test_empty_sounding_keeps_a_decimated_window(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=(), seconds=60)
    assert echo_window.read(path)["decimation"] > 1, "an empty sounding kept the whole band"


def test_floor_rising_toward_the_band_edges_keeps_a_decimated_window(capture, tmp_path):
    _, path = standalone(capture, tmp_path, echoes=(), seconds=60, edge=np.random.uniform(0.8, 1.2))
    assert echo_window.read(path)["decimation"] > 1, "a floor rising toward the band edges kept the whole band"


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

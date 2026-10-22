import os
import time

import numpy as np

import echo_window
import lfp_products
import recut_windows


def archived(capture, **kwargs):
    """A capture's power archive, and the capture itself."""
    lfs = capture(**kwargs)
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    return lfs, archive


def accidental(capture, monkeypatch, **kwargs):
    """An archive whose window kept the whole band, as auto_gate made it before
    issue 4 by opening at the band edge, with its capture already deleted."""
    lfs, archive = archived(capture, **kwargs)
    with monkeypatch.context() as patch:
        patch.setattr(echo_window, "auto_gate", lambda power, vrange: (-59000.0, -57000.0))
        echo_window.write(lfs, into=archive)
    os.remove(lfs)
    return archive


def snr(archive):
    import h5py
    with h5py.File(archive, "r") as fh:
        return fh["SNR"][()]


def content(path):
    with open(path, "rb") as fh:
        return fh.read()


def test_whole_band_window_is_recut_decimated(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch)
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert echo_window.read(archive)["decimation"] > 1, "the re-cut window still keeps the whole band"


def test_recut_archive_shrinks(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch, seconds=np.random.randint(8, 16))
    before = os.path.getsize(archive)
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert os.path.getsize(archive) < before / 4, "the re-cut archive did not shrink from %d bytes" % before


def test_recut_leaves_the_ionogram_alone(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch)
    before = snr(archive)
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert np.array_equal(before, snr(archive), equal_nan=True), "the re-cut changed the archive's SNR"


def test_recut_window_keeps_an_echo_outside_the_geometry(capture, monkeypatch, tmp_path):
    distance = np.random.uniform(1200.0, 2000.0)
    archive = accidental(capture, monkeypatch, echoes=((distance, np.random.uniform(0.3, 0.6), 0.0),), seconds=20)
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    window = echo_window.read(archive)
    assert window["range_lo_km"] <= distance <= window["range_hi_km"], "the re-cut window %.0f..%.0f km lost the echo at %.0f km" % (window["range_lo_km"], window["range_hi_km"], distance)


def test_recut_window_names_the_original_capture(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch, name="cyprus1_20260204_%06d.lfs" % np.random.randint(0, 235959))
    name = echo_window.read(archive)["source_name"]
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert echo_window.read(archive)["source_name"] == name, "the re-cut window does not name the capture %s" % name


def test_recut_keeps_the_archive_time(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch)
    then = int(time.time()) - np.random.randint(3600, 864000)
    os.utime(archive, (then, then))
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert os.stat(archive).st_mtime == then, "the re-cut archive lost its time"


def test_recut_leaves_no_scratch_behind(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch)
    recut_windows.recut(archive, str(tmp_path / "scratch"))
    assert os.listdir(str(tmp_path / "scratch")) == [], "the re-cut left its rebuilt capture behind"


def test_dry_run_leaves_the_archive_alone(capture, monkeypatch, tmp_path):
    archive = accidental(capture, monkeypatch)
    before = content(archive)
    recut_windows.main([str(tmp_path), "--scratch", str(tmp_path / "scratch")])
    assert content(archive) == before, "a dry run changed the archive"


def test_apply_recuts_every_whole_band_window(capture, monkeypatch, tmp_path):
    days = ["2026.02.%02d" % day for day in range(1, np.random.randint(3, 5))]
    for day in days:
        (tmp_path / day).mkdir()
    archives = [accidental(capture, monkeypatch, name="%s/cyprus1_20260204_120010.lfs" % day) for day in days]
    recut_windows.main([str(tmp_path), "--apply", "--scratch", str(tmp_path / "scratch")])
    assert all(echo_window.read(a)["decimation"] > 1 for a in archives), "--apply left a whole-band window behind"


def test_full_voltage_kept_on_purpose_is_left_alone(capture, tmp_path):
    lfs, archive = archived(capture)
    echo_window.write(lfs, into=archive, full=True)
    before = content(archive)
    recut_windows.main([str(tmp_path), "--apply", "--scratch", str(tmp_path / "scratch")])
    assert content(archive) == before, "a full voltage kept on purpose was re-cut"


def test_decimated_window_is_left_alone(capture, tmp_path):
    lfs, archive = archived(capture)
    echo_window.write(lfs, into=archive)
    before = content(archive)
    recut_windows.main([str(tmp_path), "--apply", "--scratch", str(tmp_path / "scratch")])
    assert content(archive) == before, "a decimated window was re-cut"


def test_archive_without_a_window_is_left_alone(capture, tmp_path):
    _, archive = archived(capture)
    before = content(archive)
    recut_windows.main([str(tmp_path), "--apply", "--scratch", str(tmp_path / "scratch")])
    assert content(archive) == before, "an archive without a window was changed"

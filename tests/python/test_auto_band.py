import shutil
import time
from types import SimpleNamespace

import lfp_products
import pytest
import rx_dechirp


@pytest.mark.parametrize(("echo_top", "previous_echo_top", "expected_move"), [
    pytest.param(24e6, None, 0, id="an echo with room keeps the default band"),
    pytest.param(27e6, None, 1.5e6, id="an echo near the edge moves the band up"),
    pytest.param(31e6, None, 4.5e6, id="the band stops at the end of the sweep"),
    pytest.param(27e6, 30e6, -3e6, id="a falling echo brings the band back down"),
    pytest.param(None, 30e6, -4.5e6, id="missing echo data restores the default band"),
    pytest.param(29e6, None, 3.5e6, id="the default margin puts two MHz above the echo"),
])
def test_the_band_follows_the_last_echo(echo_top, previous_echo_top, expected_move):
    if previous_echo_top is not None:
        previous = rx_dechirp.auto_centre(7.5e6, 20e6, 32e6, previous_echo_top)
    else:
        previous = 17.5e6
    chosen = rx_dechirp.auto_centre(7.5e6, 20e6, 32e6, echo_top)
    assert chosen - previous == expected_move, "the band did not move to the expected centre"


def test_a_custom_margin_leaves_more_room_above_the_echo():
    chosen = rx_dechirp.auto_centre(7.5e6, 20e6, 32e6, 27e6, margin=3e6)
    assert chosen == 20e6, "the band did not leave the configured margin above the echo"


@pytest.fixture
def sidecar(tmp_path, capture):
    lfs = capture(name="seed.lfs", seed=22)
    meta, sections = lfp_products.compute(lfs, fft_count=1024, iono_pad=1, iono_overlap=1)

    def write(station, stamp, echo_mhz):
        when = time.strptime(stamp, "%Y%m%d_%H%M%S")
        day = tmp_path / time.strftime("%Y.%m.%d", when)
        day.mkdir(exist_ok=True)
        path = day / ("%s_%s.lfp" % (station, stamp))
        lfp_products.write(str(path), dict(
            meta, tx_name=station, freq_min_mhz=7.5, freq_max_mhz=27.5,
            muf_index=-1 if echo_mhz is None else 12,
            muf_mhz=0.0 if echo_mhz is None else echo_mhz), sections)
        return path

    return write


def test_the_echo_top_comes_from_the_latest_previous_sidecar_for_the_station(tmp_path, sidecar):
    sidecar("NIC2", "20260204_235000", 26.0)
    sidecar("NIC2", "20260204_235500", 27.0)
    sidecar("other", "20260205_000000", 25.0)
    sidecar("NIC2", "20260205_001000", 25.0)
    found = rx_dechirp.previous_echo_top(tmp_path, "NIC2", 1770249900)
    assert found == 27e6, "the previous NIC2 sidecar did not supply its echo top in Hz"


def test_an_earlier_echo_is_ignored_when_the_latest_sidecar_has_no_echo(tmp_path, sidecar):
    sidecar("NIC2", "20260204_235000", 26.0)
    sidecar("NIC2", "20260204_235500", None)
    found = rx_dechirp.previous_echo_top(tmp_path, "NIC2", 1770249900)
    assert found is None, "an older echo was reused after the latest sounding had none"


def test_a_missing_sidecar_leaves_the_echo_unknown(tmp_path):
    found = rx_dechirp.previous_echo_top(tmp_path, "NIC2", 1770249900)
    assert found is None, "an echo top was reported without a sidecar"


def test_hackrf_uses_the_selected_centre_for_tuning_and_the_capture_header(tmp_path, sidecar, monkeypatch):
    sidecar("NIC2", "20260204_235500", 27.0)
    now = iter((1770249900, 1770249900, 1770249905))
    monkeypatch.setattr(rx_dechirp.time, "time", lambda: next(now, 1770249905))
    monkeypatch.setattr(rx_dechirp, "Logger", lambda *_: lambda *_: None)
    monkeypatch.setattr(rx_dechirp, "Publication", lambda *_: SimpleNamespace(announce=lambda *_: None))
    monkeypatch.setattr(rx_dechirp, "status", lambda *_: None)

    observed = []
    original_header = rx_dechirp.lfs_header

    def header(*args):
        raw = original_header(*args)
        path = tmp_path / "header.lfs"
        path.write_bytes(raw)
        observed.append(lfp_products.read_lfs_header(path)["cf"])
        return raw

    def launch(cmd, **kwargs):
        observed.append(int(cmd[cmd.index("-f") + 1]))
        raise FileNotFoundError

    monkeypatch.setattr(rx_dechirp, "lfs_header", header)
    monkeypatch.setattr("subprocess.Popen", launch)
    monkeypatch.setattr("signal.signal", lambda *_: None)
    opts = SimpleNamespace(outdir=str(tmp_path), duration=1, hackrf_workdir=str(tmp_path),
                           hackrf_gain=24, hackrf_vga=20, hackrf_amp=0, count=0, loop=False)
    station = dict(name="NIC2", cf="auto", f0=7.5e6, rate=100e3, dur=245,
                   lat=35.0, lon=34.0, rep=300, chirpt=0)
    rx_dechirp.run_hackrf(opts, dict(sample_rate=20e6, dec=625), [station])
    assert observed == [19000000, 19000000], "HackRF tuning and the header did not use the chosen centre"


def test_usrp_refuses_automatic_band_selection(capsys):
    rx_dechirp.run_live(SimpleNamespace(), {}, [dict(name="NIC2", cf="auto")])
    assert "requires HackRF" in capsys.readouterr().out, "USRP did not explain why automatic tuning was refused"


def test_a_shifted_band_does_not_reuse_an_attempted_slot():
    slot = 1770249900
    station = dict(name="NIC2", cf=22e6, rep=300, chirpt=0)
    windows = [(station, 45, 200)]
    _, sounding, _, _ = rx_dechirp.pick_next(windows, now=slot + 10, after_slots={"NIC2": slot})
    assert sounding == slot + 300, "shifting the band scheduled a second capture in the same slot"


def test_a_newer_capture_without_a_sidecar_does_not_reuse_an_older_echo(tmp_path, sidecar, capture):
    previous = sidecar("NIC2", "20260204_235000", 26.0)
    latest = previous.with_name("NIC2_20260204_235500.lfs")
    shutil.copyfile(capture(), latest)
    found = rx_dechirp.previous_echo_top(tmp_path, "NIC2", 1770249900)
    assert found is None, "an older echo was reused for a sounding without a sidecar"

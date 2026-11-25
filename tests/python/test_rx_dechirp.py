import os
import subprocess
import sys
import numpy as np
import pytest
import echo_window
import lfp_products
import rx_dechirp


class Writer(object):
    """A stand-in for python/echo_window.py that fails the way it is told to."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, error):
        self.error = error

    def write(self, lfs, into=None, full=False, check=False):
        raise self.error


class Recorder(object):
    """A stand-in for python/echo_window.py that remembers what it was asked,
    and reports its window as verified, or not."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, verified=True):
        self.asked = []
        self.checked = []
        self.verified = verified

    def write(self, lfs, into=None, full=False, check=False):
        self.asked.append(full)
        self.checked.append(check)
        problems = [] if self.verified else ["re-cut differs from the stored samples"]
        return {"status": "present", "target": into,
                "verification": (self.verified, problems, {})}


def quiet(_line):
    pass


def test_capture_is_kept_when_the_window_fails(capture):
    lfs = capture()
    writer = Writer(OSError("no space left on device %d" % len(lfs)))
    assert not rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet), "a capture was released although its echo window failed"


def test_capture_may_go_when_there_is_no_echo_to_keep(capture):
    lfs = capture()
    writer = Writer(echo_window.NoEchoError("silent sounding %s" % lfs))
    assert rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet), "a capture with nothing to keep was held back"


def test_capture_may_go_once_the_window_is_in_the_archive(capture):
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    assert rx_dechirp.cut_echo_window(lfs, archive, echo_window, quiet), "a capture was held back although its window was written"


def test_window_lands_in_the_archive(capture):
    lfs = capture()
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    rx_dechirp.cut_echo_window(lfs, archive, echo_window, quiet)
    assert echo_window.is_sound(archive)[0], "the archive holds no readable echo window after the sounder cut one"


def test_sounder_asks_for_the_full_voltage_when_told(capture):
    lfs = capture()
    writer = Recorder()
    rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet, full=True)
    assert writer.asked == [True], "the sounder did not ask for the full voltage"


def test_sounder_asks_for_the_echo_window_by_default(capture):
    lfs = capture()
    writer = Recorder()
    rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet)
    assert writer.asked == [False], "the sounder asked for the full voltage unprompted"


def test_sounder_checks_the_window_before_releasing_the_capture(capture):
    lfs = capture()
    writer = Recorder()
    rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet)
    assert writer.checked == [True], "the sounder released a capture against a window it never checked"


def test_capture_is_kept_when_its_window_fails_the_check(capture):
    lfs = capture()
    assert not rx_dechirp.cut_echo_window(lfs, lfs + ".h5", Recorder(verified=False), quiet), "a capture was released against a window that failed verification"


def test_stream_epoch_finds_the_start_behind_jittery_probes():
    start = np.random.uniform(1.79e9, 1.80e9)
    sr = np.random.uniform(2e6, 20e6)
    lag = np.random.uniform(0.001, 0.004)
    size = np.cumsum(np.random.randint(1, 1 << 18, 5000)) * 2
    seen = start + size / 2.0 / sr + lag + np.random.exponential(0.01, size.size)
    k = np.random.randint(size.size)
    seen[k] = start + size[k] / 2.0 / sr + lag
    found = rx_dechirp.stream_epoch(list(zip(seen, size)), sr)
    assert abs(found - (start + lag)) < 1e-5, "stream start %.6f was not recovered as %.6f" % (start + lag, found)


def test_stream_epoch_refuses_an_empty_capture():
    with pytest.raises(ValueError):
        rx_dechirp.stream_epoch([], np.random.uniform(2e6, 20e6))


def test_capture_fraction_follows_the_bytes_on_disk():
    wanted = np.random.randint(10 ** 6, 10 ** 9)
    got = np.random.randint(0, wanted)
    assert abs(rx_dechirp.capture_fraction(2 * got, wanted) - got / float(wanted)) < 1e-12, "fraction did not follow the bytes written"


def test_capture_fraction_never_passes_one():
    wanted = np.random.randint(10 ** 6, 10 ** 9)
    assert rx_dechirp.capture_fraction(2 * wanted + np.random.randint(2, 10 ** 6), wanted) == 1.0, "fraction ran past a full capture"


def growing(tmp_path, count):
    raw = str(tmp_path / "hackrf.cs8")
    np.random.randint(-128, 128, 2 * count).astype(np.int8).tofile(raw)
    return raw


def test_raw_dechirp_of_a_growing_file_matches_one_pass(tmp_path):
    sr, rate, dec = 2e6, np.random.uniform(5e4, 2e5), 50
    count = np.random.randint(200000, 400000)
    start = np.random.randint(0, 5000)
    want = (count - start) // dec - np.random.randint(5, 50)
    raw = growing(tmp_path, count)
    lfs = str(tmp_path / "live.lfs")
    live = rx_dechirp.RawDechirp(raw, lfs, b"h" * 512, sr, rate, dec, start, want, np.random.randint(1000, 30000))
    for size in sorted(np.random.randint(0, 2 * count, 40)):
        live.advance(int(size), False)
    live.advance(2 * count, True)
    live.close()
    x = np.fromfile(raw, dtype=np.int8).astype(np.float32).reshape(-1, 2)
    whole = rx_dechirp.Dechirper(sr, -sr / 2.0, rate, dec).feed((x[start:, 0] + 1j * x[start:, 1]).astype(np.complex64))[:want]
    assert np.abs(np.fromfile(lfs, dtype=np.complex64, offset=512) - whole).max() < 1e-3 * np.abs(whole).mean(), "the live dechirp did not match a single pass over the whole capture"


def test_raw_dechirp_stops_at_one_sounding(tmp_path):
    count = np.random.randint(100000, 200000)
    want = np.random.randint(100, count // 40)
    raw = growing(tmp_path, count)
    live = rx_dechirp.RawDechirp(raw, str(tmp_path / "live.lfs"), b"h" * 512, 2e6, 1e5, 20, 0, want, 4096)
    live.advance(2 * count, True)
    live.close()
    assert live.written() == want, "the live dechirp ran past one sounding"


def test_raw_dechirp_leaves_a_short_capture_short(tmp_path):
    count = np.random.randint(100000, 200000)
    raw = growing(tmp_path, count)
    live = rx_dechirp.RawDechirp(raw, str(tmp_path / "live.lfs"), b"h" * 512, 2e6, 1e5, 20, 0, count, 4096)
    live.advance(2 * count, True)
    live.close()
    assert live.written() < count, "a short capture was reported as a whole sounding"


def test_pps_latch_a_second_behind_the_gpsdo_counts_as_received():
    second = np.random.randint(1.7e9, 1.9e9)
    assert rx_dechirp.pps_latched(float(second) - 1e-7, float(second + 1)), "a PPS latched one second behind the GPSDO was refused"


def test_pps_latch_far_from_the_gpsdo_counts_as_missing():
    second = np.random.randint(1.7e9, 1.9e9)
    assert not rx_dechirp.pps_latched(float(np.random.uniform(0, 1e5)), float(second)), "a radio still counting from boot passed as PPS-locked"


def test_scratch_goes_to_the_first_writable_directory(tmp_path):
    memory, disk = tmp_path / "shm", tmp_path / "captures"
    memory.mkdir()
    disk.mkdir()
    assert rx_dechirp.scratch_dir((str(memory), str(disk))) == str(memory), "the capture did not go to the first writable directory"


def test_scratch_skips_a_directory_that_is_missing(tmp_path):
    disk = tmp_path / "captures"
    disk.mkdir()
    assert rx_dechirp.scratch_dir((str(tmp_path / "shm"), str(disk))) == str(disk), "the capture went to a directory that does not exist"


def test_scratch_falls_back_to_the_last_directory_when_none_exists(tmp_path):
    last = str(tmp_path / "captures")
    assert rx_dechirp.scratch_dir((str(tmp_path / "shm"), last)) == last, "the capture had no directory when none existed yet"


def ended():
    """The id of a process that has already exited."""
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    return proc.pid


def test_sweep_removes_the_capture_of_a_process_that_is_gone(tmp_path):
    raw = tmp_path / ("hackrf-%d.cs8" % ended())
    raw.write_bytes(os.urandom(97))
    rx_dechirp.sweep_scratch(str(tmp_path))
    assert not raw.exists(), "a capture left by a stopped process was not removed"


def test_sweep_removes_the_log_of_a_process_that_is_gone(tmp_path):
    chatter = tmp_path / ("hackrf-%d.log" % ended())
    chatter.write_text("fclose() done")
    rx_dechirp.sweep_scratch(str(tmp_path))
    assert not chatter.exists(), "a log left by a stopped process was not removed"


def test_sweep_keeps_the_capture_of_a_running_process(tmp_path):
    raw = tmp_path / ("hackrf-%d.cs8" % os.getpid())
    raw.write_bytes(os.urandom(53))
    rx_dechirp.sweep_scratch(str(tmp_path))
    assert raw.exists(), "the capture of a running process was removed"


def test_sweep_keeps_files_that_are_not_captures(tmp_path):
    other = tmp_path / "ad_mailbox_89128_shm"
    other.write_bytes(os.urandom(31))
    rx_dechirp.sweep_scratch(str(tmp_path))
    assert other.exists(), "a file that is not a capture was removed"

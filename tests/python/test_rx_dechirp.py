import echo_window
import lfp_products
import rx_dechirp


class Writer(object):
    """A stand-in for python/echo_window.py that fails the way it is told to."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, error):
        self.error = error

    def write(self, lfs, into=None, full=False, check=False, recut=False):
        raise self.error


class Recorder(object):
    """A stand-in for python/echo_window.py that remembers what it was asked,
    and reports its window as verified, or not."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, verified=True):
        self.asked = []
        self.checked = []
        self.recut = []
        self.verified = verified

    def write(self, lfs, into=None, full=False, check=False, recut=False):
        self.asked.append(full)
        self.checked.append(check)
        self.recut.append(recut)
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


def test_sounder_cuts_a_rule_one_window_again(capture):
    """A window an earlier selection rule chose is replaced while the capture
    is still there; the writer leaves one this rule chose alone."""
    lfs = capture()
    writer = Recorder()
    rx_dechirp.cut_echo_window(lfs, lfs + ".h5", writer, quiet)
    assert writer.recut == [True], "the sounder did not ask for an old window to be cut again"


def test_sounder_keeps_a_capture_whose_window_has_a_trace_at_its_edge(capture):
    lfs = capture()

    class Edge(Recorder):
        def write(self, *args, **kwargs):
            report = Recorder.write(self, *args, **kwargs)
            report.update(status="written", trace_at_edge=True)
            return report

    assert not rx_dechirp.cut_echo_window(lfs, lfs + ".h5", Edge(), quiet), \
        "the sounder released a capture although its window may have cut a trace"


def test_capture_is_kept_when_its_window_fails_the_check(capture):
    lfs = capture()
    assert not rx_dechirp.cut_echo_window(lfs, lfs + ".h5", Recorder(verified=False), quiet), "a capture was released against a window that failed verification"

import echo_window
import lfp_products
import rx_dechirp


class Writer(object):
    """A stand-in for python/echo_window.py that fails the way it is told to."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, error):
        self.error = error

    def write(self, lfs, into=None):
        raise self.error


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

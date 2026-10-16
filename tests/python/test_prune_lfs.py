import os
import time

import echo_window
import lfp_products
import prune_lfs


def quiet(_line):
    pass


class Writer(object):
    """A stand-in for python/echo_window.py that fails the way it is told to."""

    NoEchoError = echo_window.NoEchoError

    def __init__(self, error):
        self.error = error

    def write(self, lfs, into=None, full=False, check=False):
        raise self.error


class Unverified(object):
    """A stand-in whose window is written but does not verify."""

    NoEchoError = echo_window.NoEchoError

    def write(self, lfs, into=None, full=False, check=False):
        return {"status": "written", "target": into,
                "verification": (False, ["window power differs by 3.10 dB median"], {})}


def aged(capture, hours=2.0, **kwargs):
    """A capture `hours` old, with its sidecar and power archive but no window."""
    lfs = capture(**kwargs)
    _, _, archive = lfp_products.build_one(lfs, quiet=True, h5_archive_km=(0.0, 8000.0))
    then = time.time() - hours * 3600.0
    os.utime(lfs, (then, then))
    return lfs, archive


def test_capture_without_its_window_is_not_released(capture):
    lfs, _ = aged(capture)
    ok, why = prune_lfs.classify(lfs, os.path.getmtime(lfs), time.time(), window=True)
    assert (ok, why) == (False, prune_lfs.NO_WINDOW), "a capture whose archive has no echo window was released: %r" % why


def test_power_only_rule_still_releases_on_request(capture):
    lfs, _ = aged(capture)
    ok, why = prune_lfs.classify(lfs, os.path.getmtime(lfs), time.time())
    assert ok, "the power-only rule kept a capture with a sound archive: %s" % why


def test_sweep_cuts_the_missing_window_before_deleting(capture):
    lfs, archive = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, writer=echo_window)
    assert not os.path.exists(lfs), "an aged capture survived although its window could be cut"
    assert echo_window.is_sound(archive)[0], "the capture went, but its archive holds no readable echo window"


def test_sweep_keeps_a_capture_whose_window_fails(capture):
    lfs, archive = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, writer=Writer(OSError("no space left on device")))
    assert os.path.exists(lfs), "a capture was deleted although its echo window could not be cut"


def test_sweep_keeps_a_capture_whose_window_does_not_verify(capture):
    lfs, _ = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, writer=Unverified())
    assert os.path.exists(lfs), "a capture was deleted against a window that failed verification"


def test_silent_sounding_goes_without_a_window(capture):
    lfs, _ = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, writer=Writer(echo_window.NoEchoError("nothing stands out")))
    assert not os.path.exists(lfs), "a sounding with nothing to keep was held back for a window it cannot have"


def test_dry_run_cuts_nothing(capture):
    lfs, archive = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, apply=False, writer=echo_window)
    assert os.path.exists(lfs) and not echo_window.has_window(archive), "a dry run deleted a capture or wrote a window"


def test_sweep_stops_cutting_when_its_budget_is_spent(capture):
    lfs, archive = aged(capture)
    prune_lfs.sweep_aged(os.path.dirname(lfs), 1.0, log=quiet, writer=echo_window, budget_s=0.0)
    assert os.path.exists(lfs) and not echo_window.has_window(archive), "the sweep cut a window past its time budget"

import os
import sys

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir, os.pardir))
for lib in ("python", "tools"):
    path = os.path.join(ROOT, lib)
    if path not in sys.path:
        sys.path.insert(0, path)

import rx_dechirp  # noqa: E402

KM_PER_HZ = -3.0


@pytest.fixture
def capture(tmp_path):
    """A factory of synthetic .lfs captures at cyprus1's rates.

    Each echo is (range_km, amplitude, doppler_hz): a tone at the beat its
    range implies on the .lfs axis, -range/3 Hz at 100 kHz/s, shifted by the
    Doppler. A fourth element makes it last only that many seconds from the
    start. `edge` adds noise whose power rises toward both band edges, as the
    boxcar decimator's aliasing makes it at night: a first difference of white
    noise, nothing at the centre and 4 * edge^2 at the edges. The header comes
    from the sounder's own writer.
    """
    def make(echoes=((2700.0, 30.0, 0.0),), seconds=10, noise=1.0,
             tx="cyprus1", rx="yoshkar-ola", seed=None, name=None, edge=0.0):
        rng = np.random.default_rng(seed)
        sr = 40000.0
        n = int(seconds * sr)
        t = np.arange(n) / sr
        z = noise * (rng.standard_normal(n) + 1j * rng.standard_normal(n)) / np.sqrt(2.0)
        z = z + edge * np.diff(rng.standard_normal(n + 1) + 1j * rng.standard_normal(n + 1)) / np.sqrt(2.0)
        for echo in echoes:
            beat = echo[0] / KM_PER_HZ + echo[2]
            z = z + echo[1] * np.exp(2j * np.pi * (beat * t + rng.uniform())) * (t < (echo[3] if len(echo) > 3 else seconds))
        head = rx_dechirp.pack_lfs_header(
            tx, 34.68, 33.04, rx, 56.63, 47.89, 1770206410, 10, 12500000,
            int(seconds), 100000, 300, 0, 6000, 625, 25000000, False, 0, 0)
        path = tmp_path / (name or "%s_20260204_120010.lfs" % tx)
        with open(str(path), "wb") as fh:
            fh.write(head)
            fh.write(z.astype(np.complex64).tobytes())
        return str(path)
    return make

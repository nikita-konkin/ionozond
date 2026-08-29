# ionozond

An oblique-incidence ionospheric sounding suite: acquisition, an operator
console, and the tooling in between.

A chirp (LFM) transmitter sweeps the HF band; a receiver dechirps and records
the beat signal; the result is an **ionogram** — signal strength against
frequency and echo delay — from which the usable frequency band for a radio
path can be read.

```
transmitter sweep  ──▶  receiver  ──▶  .lfs capture  ──▶  .lfp products  ──▶  console
                       (acquisition)     80 MB           ~40 kB
```

## What is here

| Directory | Contents |
|---|---|
| `src/` | the console: Qt5 + Qwt application, the DSP, the file formats |
| `*.pro` | build targets — `ionozond` (console), `dsChirp_viewer` (capture browser), `lfp_build` (archive tool) |
| `acquisition/` | integration notes for the two sounders, which live in their own repos |
| `python/` | `lfp.py` reader, `spectrum_oracle.py` verification oracle |
| `tools/` | command-line utilities and their build scripts |
| `docs/` | format specifications, deployment, provenance |
| `tests/` | unit tests and differential tests |
| `docker/`, `gui/` | a reproducible build/run environment, browser-driven |
| `reference/` | **third-party sources, not ours to redistribute** — see below |

## Documentation

- [`docs/lfs-format.md`](docs/lfs-format.md) — the capture format
- [`docs/lfp-format.md`](docs/lfp-format.md) — the derived-products sidecar
- [`docs/deploy.md`](docs/deploy.md) — building on modern Ubuntu and on 16.04
- [`docs/running-the-gui.md`](docs/running-the-gui.md) — driving it from a browser
- [`docs/reverse-engineering.md`](docs/reverse-engineering.md) — how the console
  was recovered from the shipped binary, and every quirk that had to be
  preserved. Read this before changing anything in the DSP.

## Quick start

Nothing to install but Docker. The console runs on a virtual display inside the
container, published over noVNC, so you drive it in an ordinary browser at
**http://localhost:6080/vnc.html**. The launcher builds the image on first use,
which takes a few minutes.

```bash
gui/ionozond-gui.sh                          # Linux
```

```powershell
gui\ionozond-gui.ps1                         # Windows
```

With an archive, and for the other modes:

```bash
gui/ionozond-gui.sh --data ~/captures
gui/ionozond-gui.sh viewer --data ~/captures --capture cyprus1_20191023_071510.lfs
gui/ionozond-gui.sh both --original ../dsChirp --data ~/captures
```

`--data` is optional; without it the console starts against an empty archive.
The `original` and `both` modes run the shipped `dsChirp` binary side by side
with the reconstruction, so they need `--original <dir>` pointing at a
directory containing `bin/dsChirp` — that binary is not in this repository.

Native build:

```bash
sudo apt-get install -y build-essential qtbase5-dev qtbase5-dev-tools \
                        qttools5-dev-tools libqwt-qt5-dev libfftw3-dev
mkdir build && cd build && qmake ../ionozond.pro && make -j"$(nproc)"
```

## The two file formats

**`.lfs`** is the raw capture: a 512-byte header then interleaved float32 I/Q at
`sample_rate / dec`. One 250-second sounding is 80 MB, and at a 300-second
repetition period a single station produces roughly 23 GB per day.

**`.lfp`** is the sidecar written beside it: the gated ionogram, the
signal/noise spectrum, the power-delay profile and the band edges. Typically
**2000× smaller** than the capture it describes. The console reads it in
preference to the capture, so the heavy work happens once:

```
cyprus1_20191023_071510.lfs    76.3 MB
cyprus1_20191023_071510.lfp    37.0 kB      2112x
```

Build them for a whole archive with `lfp_build`; read them from Python with
`python/lfp.py`.

## Provenance

The console began as a reconstruction of `dsChirp`, an internal Qt application
whose source was lost. It was recovered from the shipped binary — not stripped,
so class names, method signatures and the Qt meta-object data survived — and
verified against the original: differential tests through `gdb`, byte-identical
generated configuration, and rendering compared with real captures.

`docs/reverse-engineering.md` records what was decoded, what was inferred, and
the deliberate quirks (a median that is not a median for odd n, a periodic
Hanning window, two different colour-map settings). One original bug is fixed
rather than reproduced, and is marked `DSCHIRP_FIX_GRADIENT_TYPO` in the source.

### A note on `reference/`

`reference/` holds copies of `gr-juha` and associated analysis scripts recovered
from an operator backup. They are **third-party, GPL-3.0**, kept only so the
format work can be checked against the code that produced the archive. They are
not ours to relicense and should be excluded from any public release —
the sounders live in their own repositories, for licensing reasons set out in
`acquisition/README.md`.

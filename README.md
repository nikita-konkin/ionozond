# ionozond

An oblique-incidence ionospheric sounding suite: the sounder, an operator
console, and the tooling in between.

A chirp (LFM) transmitter sweeps the HF band; a receiver dechirps and records
the beat signal; the result is an **ionogram** — signal strength against
frequency and echo delay — from which the usable frequency band for a radio
path can be read.

```
transmitter sweep ──▶ USRP ──▶ rx_dechirp.py ──▶ .lfs capture ──▶ .lfp sidecar ──▶ console
                               (the sounder)     80 MB, deleted   ~200 kB
                                                 once archived
                                                      │
                                                      └──▶ .h5 archive ──▶ NAS, ionograms-handler
                                                           power 1.25 MB
                                                           + echo window 1.57 MB
```

## What is here

| Directory | Contents |
|---|---|
| `src/` | the console: Qt5 + Qwt application, the DSP, the file formats |
| `*.pro` | build targets — `ionozond` (console), `dsChirp_viewer` (capture browser), `lfp_build` (archive tool) |
| `tools/rx_dechirp.py`, `tools/sounder.sh` | the sounder: Python 3, numpy and python3-uhd, no GNU Radio |
| `python/` | `lfp_products.py` sidecar builder, `h5_archive.py` and `echo_window.py` archive writers, `lfp.py` and `lfs_info.py` readers, `spectrum_oracle.py` verification oracle |
| `tools/` | archive upkeep, diagnostics, systemd units, the desktop launcher, build scripts |
| `tools/host-setup/` | bringing up a sounder host: network, USRP, tuning, clock, service |
| `acquisition/` | integration notes for chirpsounder2, which lives in its own repository |
| `docs/` | format specifications, deployment, the physics reference, provenance |
| `tests/` | C++ unit and differential tests; Python tests in `tests/python/` |
| `docker/`, `gui/` | a reproducible build/run environment, browser-driven |
| `reference/` | **third-party sources, not ours to redistribute** — see below |

## Documentation

- [`docs/ionogram-physics.html`](docs/ionogram-physics.html) — *Oblique Chirp
  Ionograms*: the whole chain from the ionosphere to the pixel, the recording
  window, parallel sounding, finding a sounder's timing, group-delay
  dispersion, the error budget. Open it in a browser.
- [`docs/gdd-demo.html`](docs/gdd-demo.html) and
  [`docs/gdd-playground.html`](docs/gdd-playground.html) — interactive models
  for the group-delay dispersion part
- [`docs/lfs-format.md`](docs/lfs-format.md) — the capture format
- [`docs/lfp-format.md`](docs/lfp-format.md) — the derived-products sidecar
- [`docs/deploy.md`](docs/deploy.md) — building on modern Ubuntu, on 16.04,
  and natively on a sounder host
- [`tools/host-setup/README.md`](tools/host-setup/README.md) — bringing up a
  sounder host you can only reach remotely
- [`docs/running-the-gui.md`](docs/running-the-gui.md) — driving the console
  from a browser
- [`docs/reverse-engineering.md`](docs/reverse-engineering.md) — how the console
  was recovered from the shipped binary, and every quirk that had to be
  preserved; also the `.h5` archive, the echo window and pruning. Read this
  before changing anything in the DSP.

The practicum at <http://konkin-nikita.ru/ionozond-playground/>
([source](https://github.com/nikita-konkin/ionozond-playground)) has
interactive models built on these documents, in Russian and English.

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

The container cannot run the sounder: it has no UHD and mounts the archive
read-only. On a sounder host, build natively:

```bash
bash tools/build-native.sh --run
```

Native build by hand:

```bash
sudo apt-get install -y build-essential qtbase5-dev qtbase5-dev-tools \
                        qttools5-dev-tools libqwt-qt5-dev libfftw3-dev
mkdir build && cd build && qmake ../ionozond.pro && make -j"$(nproc)"
```

## The console

- **One panel per transmitter**, in a scrolling stack. Each panel can be
  stretched on its own, and its caption gives the capture time in UTC and
  Moscow time.
- **The ionogram is drawn during reception**, left to right, as the sweep
  arrives. Turn it off with *Строить ионограмму во время приёма*.
- **A finer picture**: zero-padded, overlapped spectra, with the FFT length
  and the picture detail set per station.
- **Any ionogram opens in a window of its own**, and the control ionogram
  panel browses the archive.
- **Daily-course panels** (*Суточный ход*) of SNR and the power-delay profile,
  with session rows, a CPU plot and a disk gauge beside them.
- **The schedule dialog writes the configuration**, accepts fractional
  chirptime and sweep rate, and says when a restart is needed.
- **The schedule and parameters are checked** for colliding sessions and bad
  values, with advice on how to fix each one.
- **START and STOP drive the sounder.** The console refuses to start a second
  sounder against the same radio, warns when a change cannot reach a running
  service, and stops its sounder when it closes.

The parameters dialog also holds what the station keeps: the `.h5` archive,
whether and how long to keep the `.lfs` captures, and the NAS destination with
how long `.h5` files stay local.

## The sounder

`tools/rx_dechirp.py` receives 25 MS/s from the USRP, dechirps it against the
transmitter's sweep, decimates by 625 to 40 kHz and writes each sounding as an
`.lfs` capture. It is a port of gr-juha's `chirp_downconvert` to plain Python;
see its module docstring for the algorithm. Check it without a radio first:

```bash
python3 tools/rx_dechirp.py --self-test
```

After each sounding it builds the `.lfp` sidecar and the `.h5` archive in the
same pass, appends the echo window to the archive, and then deletes the capture
— but only once the archive and the window both read back. Set `keep_lfs` to
keep captures, for a number of hours or indefinitely.

Two ways to run it, never both at once, since both open the same radio:

- **From the console**: set `tools/sounder.sh` as *Программа зондирования* and
  press START.
- **Unattended**, as `ionozond-sounder.service`. Install it with
  `tools/host-setup/18-install-service.sh`, which writes the site settings to
  `/etc/default/ionozond`.

### Archive upkeep and diagnostics

| Tool | Does |
|---|---|
| `tools/prune_lfs.py` | deletes captures whose products are proven, oldest first, between a floor in days and a ceiling in free GB; `ionozond-prune.timer` runs it |
| `tools/nas_sync.py` | copies products to a NAS and deletes local `.h5` files only once the NAS holds identical copies; `ionozond-nas.timer` runs it |
| `tools/find_trace.py` | profiles `.h5` archives over 0–8000 km and says where the echo is, when a new station's panel shows nothing |
| `tools/rx_spectrum.py` | shows the HF band the receiver sees, as text, before anything is dechirped |
| `python/echo_window.py` | cuts the echo window from old captures, standalone or into their archives |
| `tools/host-setup/19-install-desktop.sh` | puts the console in the desktop menu |

## File formats

**`.lfs`** is the raw capture: a 512-byte header then interleaved float32 I/Q at
`sample_rate / dec`. One 250-second sounding is 80 MB, and at a 300-second
repetition period a single station produces roughly 23 GB per day. The sounder
deletes it once the archive is written, unless told to keep it.

**`.lfp`** is the sidecar written beside it: the gated ionogram, the
signal/noise spectrum, the power-delay profile and the band edges. The console
reads it in preference to the capture, so the heavy work happens once. At the
original grid it is about 2000× smaller than the capture; the finer picture
brings it to about 200 kB.

```
cyprus1_20191023_071510.lfs    76.3 MB
cyprus1_20191023_071510.lfp    37.0 kB      2112x
```

Build them for a whole archive with `lfp_build` or `python/lfp_products.py`;
read them from Python with `python/lfp.py`.

**`.h5`** is chirpsounder2's `lfm_ionogram-*.h5`, so ionograms-handler reads
it with no new code. It holds two things:

- **the power spectrogram**, 0–8000 km of delay, median-normalised: 1.25 MB,
  enough to rebuild any ionogram that does not need a different FFT length;
- **the echo window**, an `echo_window` group with the dechirped voltage around
  the echoes: 1.57 MB on cyprus1. Power keeps where the echoes are; the window
  keeps their Doppler, fading and phase.

The window follows ionograms-handler's `docs/echo-window.md`, version 1, and
on real captures its samples are bit-identical to the reference
implementation's.

```bash
python3 python/echo_window.py captures/2026.02.04 --dry-run
python3 python/echo_window.py capture.lfs --into lfm_ionogram-....h5 --verify
```

## Tests

```bash
docker run --rm -v "$PWD/..":/work -v ~/captures:/data:ro dschirp-dev bash /work/ionozond/tests/run_tests.sh
```

```bash
python3 -m pytest tests/python
```

The first is the C++ suite: unit tests, and differential tests against the
original binary and a Python oracle, which need real captures at `/data`. The
second covers the echo window and the sounder's handling of it, on synthetic
captures, and needs numpy, h5py and pytest.

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
not ours to relicense and should be excluded from any public release.
`acquisition/README.md` sets out why chirpsounder2 lives in its own
repository.

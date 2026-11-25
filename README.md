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

`tools/rx_dechirp.py` receives from a USRP N210 (25 MS/s) or a HackRF One
(up to 20 MS/s), dechirps against the transmitter's sweep, decimates by `dec`
(625 gives 40 kHz at 25 MS/s) and writes each sounding as an `.lfs` capture. It is a port of gr-juha's `chirp_downconvert` to plain Python;
see its module docstring for the algorithm. Check it without a radio first:

```bash
python3 tools/rx_dechirp.py --self-test
```

After each sounding it builds the `.lfp` sidecar and the `.h5` archive in the
same pass, appends the echo window to the archive, and then deletes the capture
— but only once the archive reads back and the window verifies against the
capture: its filter, an exact re-cut, and the power it re-derives. Set
`keep_lfs` to keep captures, for a number of hours or indefinitely; a kept
capture that ages out goes by the same rule, and one whose archive still lacks
its window has one cut first.

Two ways to run it, never both at once, since both open the same radio:

- **From the console**: set `tools/sounder.sh` as *Программа зондирования* and
  press START.
- **Unattended**, as `ionozond-sounder.service`. Install it with
  `tools/host-setup/18-install-service.sh`, which writes the site settings to
  `/etc/default/ionozond`.

### Receivers

The parameters dialog's *Приёмник* chooses the radio for a console launch.
`tools/sounder.sh` gets the choice as `SOUNDER_RECEIVER` and turns it into
arguments; the radio's address always comes from `RADIO_ADDR` and `JUMBO` in
`/etc/default/ionozond`. The service ignores the dialog and runs what that file
says.

| Choice | Arguments | Time from |
|---|---|---|
| HackRF One | `--hackrf --hackrf-gain 24` | this host's clock |
| USRP N210 (host clock) | `--no-gpsdo` | this host's clock |
| USRP N210 (GPSDO) | none | the N210's GPSDO and PPS |
| HackRF One (USRP GPSDO time) | `--hackrf --hackrf-gain 24 --time-from-usrp addr=$RADIO_ADDR` | the N210's GPSDO, read over UHD |
| As configured | `SOUNDER_ARGS` from `/etc/default/ionozond` | whatever that says |

Set **Начало свипа, МГц** in the sounder's schedule to the transmitter's
sweep start in MHz. The console writes it as `f0` in Hz in `chirp_config.py`.
A blank or zero field preserves the legacy window, which assumes the sweep
starts at `centre - sample_rate / 2`. With `f0` set, either receiver starts
when the sweep reaches that lower edge and stops at the sweep end or upper
band edge, whichever comes first. The receiver skips the sounding if the
sweep starts above the lower edge or ends before it enters the band.

For NIC2, set the sweep start to 7.5 MHz. At 20 MS/s with a centre of
23.5 MHz, the receiver starts 60 s into a 100 kHz/s sweep and records
13.5–20 MHz of a 125 s sounding. The filename and header time identify the
scheduled sounding. A start that is off by `Δf` reads as a delay error of `Δf / rate`: 100 Hz at
100 kHz/s is 1 ms, or 300 km.

For automatic tuning on a HackRF, type `auto` in the console's `cf` field
(«Частота программного гетеродина USRP, кГц»). Set `f0` («Начало свипа, МГц»)
to the transmitter's sweep start and use the full sweep duration in `dur`.
The receiver starts with its lower edge at `f0` and reads the `.lfp` for the
latest sounding of that station in the receiver's dated output folders. It
moves the band to leave 2 MHz above the detected echo top, within the sweep
when the sweep is at least as wide as the receiver band. A missing sidecar
or undetected echo restores the default band. In a separately managed
`chirp_config.py`, set `auto_band_margin` in Hz on the sounder to change the margin.

The HackRF has no timestamps. The sounder starts `hackrf_transfer` two seconds
early (`--hackrf-lead`), watches the capture file grow, and takes the stream
start as the earliest time consistent with every size it saw. That discards
the jitter of USB transfers and buffered writes; what remains is their
constant part, a fraction of a millisecond on the station laptop. The `.lfs`
is dechirped while the capture arrives, so the console draws the ionogram
live; if a later size probe moves the start by more than 0.05 ms the capture
is dechirped again before its products are built. A capture that starts late,
stalls or comes up short is marked failed and nothing is published.

### Archive upkeep and diagnostics

| Tool | Does |
|---|---|
| `tools/prune_lfs.py` | deletes captures whose products are proven — the archive and its verified echo window, cut first where missing — oldest first, between a floor in days and a ceiling in free GB; `ionozond-prune.timer` runs it |
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

To keep the **full voltage** instead, set *Полный сигнал z в архиве .h5* in
the parameters dialog, or `save_raw_voltage = true` in `config.ini`. The
window then spans the whole band, undecimated: the capture sample for sample,
69 MB a sounding, so the `.h5` alone replaces the `.lfs`. It is stored as the
window's own `echo_window/z`, not as chirpsounder2's root `z`. muf reads a
root `z` with chirpsounder2's timing, which starts the sweep at 0 Hz rather
than at the capture's 7.5 MHz.

```bash
python3 python/echo_window.py captures/2026.02.04 --dry-run
python3 python/echo_window.py capture.lfs --into lfm_ionogram-....h5 --verify
python3 python/echo_window.py capture.lfs --full --out /archive/full
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

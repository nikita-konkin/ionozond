# dsChirp reconstruction — verified findings

Facts here are **confirmed**, either by differential test against the original binary,
by cross-check with the `gr-juha` sources in `reference/`, or by parsing real captures.
Anything still inferred-but-unverified is marked TODO.

Target: Qt 5.15 + Qwt 6.1, Linux, exact behavioural clone.
Original build: Qt 5.9.1, Qwt 6, FFTW3 (double), GCC 5.4 / Ubuntu 16.04, non-PIE.

## How to verify

```
docker build -t dschirp-dev -f docker/Dockerfile docker
docker run --rm -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
    dschirp-dev bash /work/ionozond/tests/run_tests.sh
```

The original binary runs in the same container and is used as a reference oracle:

- `tools/oracle_getmedian.gdb` calls functions **inside the shipped binary** under gdb
  (it is not stripped and not PIE, so symbols resolve). Because there is no DWARF,
  calls must be cast: `set $f = (double (*)(double*, int*))getMedian`.
- `tests/run_oracle.sh` runs the whole GUI headless with fixture settings.

## Verified: `.lfs` file format

512-byte packed header, then a raw stream of `std::complex<float>` (interleaved
float32 I/Q) at `if_rate = sample_rate / dec`. Struct in `src/lfs_header.h`, taken from
`gr-juha/include/juha/lfs_header.h` and confirmed field-by-field against two real
80 MB captures (`test_lfs_header`: 31 offset assertions + parse).

`lfsheader_read` rejects anything whose `format_ver != 1.0f` or `header_size != 498`.
The newer `chirpsounder/lfs_header.py` writes `ver 1.1 / size 512`, so files from it are
**refused by this app**. Reproduced deliberately.

## Verified: spectrum pipeline

`QRxIonogram::readNewLfsSpecData` @0x46e6f0, reimplemented in `src/igmath.cpp` and
cross-checked against an independent NumPy implementation
(`python/spectrum_oracle.py`) on a real capture — agreement to **1.7e-13** relative,
i.e. FFT summation order only.

```
z[k]     = w[k] * (I[k] + j*Q[k])     periodic Hanning
S        = fft(z)                      batched, FFTW_FORWARD | FFTW_ESTIMATE
power[k] = re^2 + im^2
power    = fftshift(power)             half = (n+1)/2
power   /= getMedian(power)            noise-floor normalisation
power    = reverse(power)              delay axis flip
```

Frame-level: display floor is the **5th percentile** of all normalised power
(`nth_element` at `total * 5.0 / 100.0`); `maxDb = 10*log10(max)`; colour scale is
`setLevelInterval(0.125 * maxDb, maxDb)`.

## Quirks that must be preserved

These are all deviations from what a reimplementation would naturally write. Each is
reproduced on purpose.

1. **`getMedian` is not a median for odd n.** @0x471c80. It always computes
   `(max(lower half) + nth_element[n/2]) * 0.5` with no parity branch. Confirmed by
   calling the original under gdb: for `{5,1,9,3,7,2,8}` with n=7 it returns **4**,
   where a true median is 5. Spectrum lengths are powers of two so this never bites in
   practice, but it is reproduced exactly (`test_igmath`).

2. **Hanning window is periodic, divided by n not n-1.** @0x479a90.
   `w[k] = 0.5*(1 - cos(2*pi*k/n))`. This is *not* `numpy.hanning()`; it matches
   `scipy.signal.get_window('hann', n, fftbins=True)`. The step `2*pi/n` is narrowed to
   `float` before multiplying by `(float)k`, which is reproduced so the window matches
   bit for bit.

3. **`colormap_gradinet` — a live bug in the original, NOW FIXED.** Confirmed by xref:
   - `ParametersDialog::WriteSettings()` @0x457694 writes `colormap_gradient`
   - `ParametersDialog::ReadSettings()`  @0x458aee reads  `colormap_gradient`
   - `frmMain::getBaseSoundParams()`     @0x44f06e reads  `colormap_gradinet`  <-- typo

   So the checkbox round-trips correctly in the dialog, but the value frmMain puts into
   `QBaseSoundParams` (which drives the ionogram colour map) reads a key that is never
   written and therefore always falls back to its default. The user's gradient choice has
   no effect on the plots.

   **Fixed at the user's request**: `frmMain::getBaseSoundParams()` now reads
   `colormap_gradient`, so the checkbox takes effect. Marked `DSCHIRP_FIX_GRADIENT_TYPO`
   in `src/frmmain.cpp`, with the original behaviour recorded in the comment.

   **A second deliberate difference**, found when the generated config was first fed
   to a real host: `ParametersDialog` writes the "длительность импульса" field back
   verbatim, and `buildChirpConfig` emits every `[General]` key as `key = value`. When
   `tb` is absent or blank — as it is in any freshly created `config.ini` — the result
   is a bare `tb = `, which is a **Python syntax error**. The whole generated
   `chirp_config.py` then fails to import, so the sounder cannot start at all:

   ```
   ssas = 1
   tb =
   whiten = False
   ```

   Real deployments carried `tb = 0` (see `reference/gr-juha__apps__chirpsounder__chirp_config.py`),
   which is why the original never tripped over it. An empty `tb` now defaults to `0`,
   marked `DSCHIRP_FIX_EMPTY_TB` in `src/parametersdialog.cpp`, and `tb=0` was added to
   `tests/fixtures/config.ini` so the shipped default produces a file that parses.

4. **There are TWO colour-map settings, and they are different maps.**
   `IG_COLORMAP_LIST` is built at @0x41a075 by appending the map vectors in
   construction order (no reversal, unlike the QStringList combos):

       0 BLUE_BASE   1 WHITE_BASE   2 IG2      3 IG2_MOD   4 GRAY
       5 RAINBOW     6 RAINBOW_WHITEBASE       7 IG2_BASE  8 IG2_MOD_BASE

   `PDP_COLORS` is deliberately absent — it belongs to the power-delay-profile
   widget, not the selector.

   - `ig_colormap_index` (=1 as shipped) drives the **ionogram**
     → WHITE_BASE_COLORS, giving the white background of the manual's plots.
   - `colormap_index` (=8 as shipped) drives the **variation plots**
     → IG2_MOD_BASE_COLORS, whose bar runs magenta → blue → green → yellow →
     red, matching the colour bar in the manual's screenshots.

   Using `colormap_index` for the ionogram (which is what a casual reading
   suggests) gives visibly wrong colours.

5. **Combo-box lists are stored ascending, opposite to construction order.** The
   static initialisers build the QStrings descending then append them ascending:
   - `FFT_COUNT_LIST` = 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
     → `fft_count_index=5` is **16384**, matching the manual.
   - `SAMPLE_RATE_LIST` = 3125, 6250, 12500, 25000
     → `sample_rate_index=3` is **25000**, matching `sample_rate=25000` in config.ini.
   - `IG_VERTICAL_SCALE_LIST` = "h, km", "t, ms"
     → `ig_vertical_scale_index=1` is **"t, ms"**, which is what the original's
     Parameters dialog showed for that setting.
   All three index cross-checks only work with this ordering.

## Derived products (SNR, LUF/MUF, PDP)

### calculateUsageFrequencies @0x478980

Finds the usable band by looking for a run of three consecutive spectra that
each contain at least three consecutive non-zero points (non-zero meaning
"survived the noise gate"):

- forward pass -> on the third qualifying spectrum, `lufIndex = s - 2`
- backward pass -> on the third qualifying spectrum, `mufIndex = s + 2`

Both indices start at -1, and the whole function is skipped when there are
fewer than three spectra or three points.

### calcAllSpecSnr @0x479d30

One SNR figure per spectrum, in dB:

```
noise = specMedianPower * 1.3862943611198906      // 2*ln(2)
sum   = total of every point > 0
snr   = 10*log10(sum/noise - 1)                   // only when the ratio >= 1
```

Spectra outside [lufIndex, mufIndex] get 0, as do spectra whose median is zero
or whose ratio falls below 1.

The sum must be taken over the **linear** normalised power, not the dB array:
each spectrum was divided by its own median in `buildSpectra`, so the median is
1 by construction and the noise estimate reduces to `2*ln(2)`.

### Frequency-band averaging

`getMinFreqBand()` @0x478bf0 returns `(freqMax - freqMin) * 1e6 / (specCount - 1)`,
the Hz covered by one spectrum. The "усреднение по частоте (кГц)" setting
averages the per-spectrum SNR over bands of that many spectra; a band narrower
than one spectrum has no effect.

### Power-delay profile

`getIntegratedPowerDelayProfile` @0x479350 is only a cached accessor; the work
is done in `calcShortAndIntegratedPowerDelayProfiles` @0x47ad60, now decoded
for the integrated half:

- power is summed across spectra for each delay row, but **only across spectra
  inside the usable band**, between `getLufIndex()` and `getMufIndex()` —
  frequencies outside the band contribute nothing
- each sample covers a delay interval **centred** on its row: the original
  takes `getValueY(i+1) - getValueY(i)` and halves it (the 0.5f @0x494478)

Both corrections were made after an initial inferred version that summed over
every spectrum and used left-aligned bins.

Still not decoded: the companion "short" profile, which uses a separate
`QShortPowerDelayProfileSample` type and is not needed by the ПЗМ panel.

### Second noise gate

`deleteObjectsUnderNoiseLevel(dst, src)` @0x478700 applies a statistical gate
after the Rosin one: for every spectrum that has any positive value, apply
`applyHardPowerLimitToSpec` at `specMedianPower * 2*ln(2)` — the same
1.3862943611198906 constant as `calcAllSpecSnr`.

**Unit caveat.** The threshold is a multiple of the median POWER, which implies
`QIonogramDataArray` holds linear normalised power rather than dB. This
reconstruction keeps its arrays in dB, so the caller passes the equivalent
level, `10*log10(2*ln 2) = 1.416 dB` for median-normalised spectra. That is
milder than the Rosin threshold, so this stage removes little on top of it —
which is consistent, but means it is not independently exercised.

## Verified: generated `chirp_config.py`

`frmMain::CreateConfigFile()` emits Python from the two `.ini` files. Rules recovered
from the disassembly and matched literal-for-literal against the real
`reference/gr-juha__apps__chirpsounder__chirp_config.py`:

- `true`/`false` → `True`/`False`
- `sample_rate` and `rate` are written in `NNNe3` form
- `if_rate` is emitted literally as `sample_rate/dec`
- string values are quoted (` = "`)
- `config_file` and `sound_app` are **skipped** — they are dsChirp's own settings and
  do not appear in the generated file (confirmed: the real file has neither)
- each active station becomes a dict inside `sounders = [ ... ]`; the receiver becomes
  `rx_station = {'name':..,'lat':..,'lon':..}`; the file ends with `get_all_sounders()`

## Axis geometry

Binary constants agree exactly with `reference/ionogr_clean__ig_utils1.py`:

- `freq_start = cf - sample_rate/2`, `freq_stop = freq_start + dur*rate`
  (7.5…32.5 MHz for cyprus1, matching the manual's screenshots)
- virtual range `= 3e8 * (±if_rate/2) / rate / 1e3` km → the `VIRT_HEIGHT_MIN/MAX`
  = ∓60000 constants found in `.rodata`
- `VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT = 180`, `VIRT_HEIGHT_WINDOW_KM_DEFAULT = 1500`
- ionogram window: `rmin = ray_distance(tx→rx) - 180`, `rmax = rmin + 1500 + 180`

`earthDistanceKm` in `src/common.cpp` matches `reference/ionogr_clean__distance.py`
line for line, down to `EARTH_RADIUS_KM = 6378.137` and `LAY_E_SQR = 40000`
(reflection at 100 km, since `sqrt(d² + (2·100)²)`).

**It is fed coordinates in the wrong units, and that is faithful to the
original.** The header's `lat`/`lon` are degrees and minutes as `DD.MM`, not
decimal degrees; every reader in the chain multiplies them by `pi/180` anyway.
Confirmed against a GPS fix taken at the receiving site with the site's own
GPSDO — 1.3 km from the `DD.MM` reading, 34.6 km from the decimal one — and
against the measured echo, whose leading edge sits +20.9 km above the `DD.MM`
ray distance versus +56.0 km above the decimal one. Full evidence in
[`lfs-format.md`](lfs-format.md). Not corrected here: it would move every
existing display, and the call belongs to the archive's owner.

## Tuning ionogram quality

Three settings decide what an ionogram looks like. Measured on a real capture
(`cyprus1_20260204_000010.lfs`, whitened, 25 MS/s):

### The speckle filter — `obj_level`, `obj_size_horizontal`, `obj_size_vertical`

A point survives only if it has `obj_level` neighbours inside an
`obj_size_horizontal` x `obj_size_vertical` window. The original hard-coded
9 x 3 and 11; all three are now read from `config.ini`, because the right
value depends on the path and on the interference at a site.

| setting | surviving points | LUF | MUF |
|---|---|---|---|
| level 6 | 35099 | 7.66 | 32.34 |
| level 8 | 28604 | 7.66 | 32.21 |
| **level 11 (default)** | **14028** | **7.66** | **31.84** |
| level 14 | 4179 | **8.07** | **28.11** |
| window 9x5 | 33227 | 7.66 | 32.34 |

Lower keeps more of a faint trace and more noise; higher gives a sparser but
cleaner one. Note what the band edges do: at level 11 the LUF/MUF span nearly
the whole sweep, which means noise is still being counted as signal. Stricter
cleaning narrows it to something physically plausible — worth remembering that
LUF and MUF are only as good as the gate that feeds them.

### FFT length — `fft_count`, in the parameters dialog

Already adjustable. It trades frequency resolution against delay resolution and
nothing else; there is no "better" setting, only a choice of which axis matters:

| fft_count | spectra | frequency step | delay bin | rows in the window |
|---|---|---|---|---|
| 8192 | 1220 | 20 kHz | 14.65 km | 117 |
| **16384 (default)** | **610** | **41 kHz** | **7.32 km** | **232** |
| 32768 | 305 | 82 kHz | 3.66 km | 460 |

16384 matches the sweep: at 100 kHz/s the transmitter moves 41 kHz during the
0.41 s each spectrum integrates, so the frequency step and the sweep rate agree.
Going coarser in one axis to gain the other is a real choice, but leaving it
matched is the reason the default is where it is.

### Sidecars must be rebuilt

The console reads the `.lfp` in preference to the capture, so changing any of
this leaves old ionograms rendered the old way until their sidecars are
regenerated:

```bash
python3 python/lfp_products.py ~/ionograms --recurse --force --obj-level 14
```

## Structural deviation from the original

The numeric kernel lives in `src/igmath.{h,cpp}` rather than inline in `QRxIonogram`
and `QIonogram`, so it can be tested without linking Qt/Qwt. Behaviour is identical;
only the placement differs. Everything else follows the original's translation-unit
layout, recovered from the `_GLOBAL__sub_I_<file>.cpp` symbols.


## Verified: ionogram rendering

`tools/render_ionogram.cpp` runs the whole chain headless and writes a PNG
(`tools/render.sh` builds and invokes it). On `cyprus1_20191023_071510.lfs` it
produces a recognisable oblique ionogram: two traces at ~8.9-9.1 ms spanning
~9 to ~19 MHz with the usual nose near the MUF and a 35.7 dB peak at
18.95 MHz — matching the ionograms in the user manual.

### Row order: why the original reverses the spectrum

The delay-axis orientation is easy to get backwards and nothing in the
disassembly states it outright. Settled empirically by profiling mean power per
row (`RENDER_PROFILE=1`): the returned signal sits at rows 8554..8575.

- rows ASCEND with delay → that band is +2655..+2809 km, just beyond the
  2550 km ray path. Correct.
- rows DESCEND with delay → the same band is −2684 km. Unphysical.

The physical reason: dechirping against a local replica running ahead of the
received sweep drives the beat frequency negative as echo delay grows, so after
`fftshift` the rows run long-delay-first. The `reverse` in
`readNewLfsSpecData` puts them back into ascending-delay order. It is not
cosmetic and must not be dropped.

### Noise gate

`QIonogram::getPowerDynamicLimit` @0x477a90 is a **Rosin (triangle)
threshold**, reconstructed in `igmath.cpp`:

1. bin the dB values into integer bins, negatives folded into bin 0
2. convert the histogram to a survival curve S(d) = count of points >= d
3. return the d maximising `N - (N/limit)*d - S(d)` — the bin where the curve
   falls furthest below the chord from (0,N) to (limit,0)

`applyHardPowerLimitToSpec` @0x478520 then zeroes everything strictly below it.

The threshold must be computed over the **whole** spectrum, not the cropped
delay window: 231 windowed rows do not sample the noise distribution well
enough and the threshold comes out near zero.

### Colour scale caveat

Scaling colours to the frame-wide maximum (what
`setLevelInterval(0.125*maxDb, maxDb)` implies) flattens the display whenever
the direct signal falls outside the delay window — the visible band then
occupies the bottom sixth of the scale and reads as pure noise. The original
avoids this through `QIonogram` post-processing that is not yet implemented
(`applyPowerLimit`, `deleteObjectsUnderNoiseLevel`, direct-signal cutting).
The render tool scales to the window maximum for now; `RENDER_GLOBAL_SCALE=1`
restores the frame-wide behaviour for comparison.

## Status

| Component | State | Verified against |
|---|---|---|
| `src/lfs_header.*` | done | 31 offset assertions + 2 real 80 MB captures |
| `src/igmath.*` | done | original binary via gdb, + independent NumPy |
| `src/configwriter.*` | done | **byte-identical** to the original's output |
| `src/schedule.*` | done | original's session panel + real capture filenames |
| `src/common.*` | done | combo lists confirmed in the original's dialog |
| `src/frmmain.*` + `frmmain.ui` | shell builds and runs | screenshot vs original |
| `src/qigcolormap.*` (10 maps, 33 colours) | done | extracted from `.rodata` |
| ionogram raster pipeline | done | renders a real trace from a real capture |
| `src/rasterdata.*` | done | index mapping taken from @0x472380 |
| `src/qrxionogram.*` (Qwt plot) | done | renders a real capture, axes match |
| `src/qigframe.*` (2x2 panel group) | done | layout matches the original |
| `src/parametersdialog.*` | done | screenshot vs the original's dialog |
| `src/scheduledialog.*` | done | screenshot vs the manual |
| `QSessionInfoWidget` / `QCpuUsageWidget` / `QDrivePieChart` | done | screenshot vs original |
| `src/snrvariationswidget.*` | done | renders from real captures |
| `src/pdpvariationswidget.*` | done | renders from real captures |
| `src/datetimescaledraw.*` | done | formats recovered from `.rodata` |
| `getPowerDynamicLimit` + `applyHardPowerLimitToSpec` | done | unit tests |
| `calculateUsageFrequencies` (LUF/MUF) | done | decoded from @0x478980 |
| `calcAllSpecSnr` | done | decoded from @0x479d30 |
| SNR frequency-band averaging | done | `getMinFreqBand` @0x478bf0 |
| integrated power-delay profile | done | decoded from @0x47ad60 |
| `deleteObjectsUnderNoiseLevel` | done | decoded from @0x478700 |
| remaining QIonogram analytics (~17 methods) | not started | |
| deployment docs (modern + 16.04) | done | see `DEPLOY.md` |
| `.lfp` sidecar format + reader/writer | done | round-trip test + Python cross-read |
| `lfp_build` archive tool | done | 747x on real captures |
| GUI harness (noVNC, 4 modes) | done | see `gui/README.md` |

Resources were extracted straight out of the binary's Qt resource bundle
(`analysis/extract_qrc.py`): the four PNG icons, the app `.ico`, and the factory
default `config.ini` / `schedule.ini` are the originals, not recreations.

### One more original quirk

The shipped factory `config.ini` (`res/ini/config.ini`, extracted from the binary)
writes `sound_app_dir=...`, but every code path reads and writes `sound_app`. On a
genuinely fresh install the sounder path therefore starts out empty and has to be set
in the Parameters dialog before a session can run. Left as-is.

### Driving the original

`tests/drive_oracle.sh` runs the shipped binary under Xvfb and clicks through it with
xdotool. Two things are needed to get it as far as generating a config:

- Cyrillic fonts must be installed or the whole UI renders as `?` boxes.
- `ParametersDialog` refuses to accept unless the configured `config_file` path
  already exists, so the fixture pre-creates it. Until the dialog has been accepted
  once, `frmMain`'s copy of the filename is empty and Start silently does nothing --
  the filename only reaches frmMain through the `changeConfigFileName` signal.


## New: the .lfp derived-products sidecar

Not part of the reconstruction — a forward step agreed with the user. Spec in
`docs/lfp-format.md`, C++ in `src/lfpfile.*`, Python reader in `python/lfp.py`,
archive tool in `tools/lfp_build.cpp`.

Measured on real captures: **76.3 MB -> 108 kB, a 747x reduction**, ~330 ms per
capture to build. The viewer now prefers the sidecar and writes one when it is
missing, so a capture costs 80 MB and 610 FFTs exactly once.

Three `.lfs` mistakes are deliberately not repeated:

1. the version is two `uint16`s with a stated compatibility rule, not a `float`
   compared for exact equality
2. `header_size` always means the whole header, with one writer definition
3. every parameter that changes the output is stored in the file

`lfs_list()` in the operator's own scripts filters on `.endswith(".lfs")`, and
`ig_cleaning.py` writes its PNGs to a separate destination directory, so a
`.lfp` beside each capture does not disturb the existing tooling.

### Open issue: LUF/MUF are near-useless on gated-but-uncleaned data

On a real capture the decoded `calculateUsageFrequencies` returns
LUF 7.5 MHz / MUF 32.25 MHz — essentially the whole sweep. That is the
algorithm behaving as decoded: the run-detector only needs three consecutive
spectra with three consecutive non-zero points, and gated noise still leaves
scattered points across the whole band.

This is the reason the un-implemented cleaning stages matter. `deleteSmallObjects`,
`fillMask` and `medianEqualize` exist precisely to remove that speckle, and
until they are implemented the band edges, the SNR band gating and the PDP's
luf..muf restriction are all effectively inert. Worth doing before trusting any
of the three.

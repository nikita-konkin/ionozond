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

**What 2·ln(2) is.** Not the median-to-mean conversion for an exponential it is
usually described as: that is 1/ln2 = 1.4427, since the median of an
exponential is `mean·ln2`. 2·ln2 = 1.3863 sits 3.9% below it (−0.17 dB), close
by the coincidence that 2·ln²2 = 0.961 rather than by derivation. For
exponential noise a threshold at `F · median` passes exactly `2^-F` of pure
noise, so this gate passes 38.3% of it — the quantitative reason it removes so
little. The value is the original's and is reproduced as such; only the
explanation was wrong.

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
9 x 3 and 11; all three are now read from `config.ini` and settable in the
parameters dialog under **Очистка ионограммы**, because the right value depends
on the path and on the interference at a site.

| setting | surviving points | LUF | MUF |
|---|---|---|---|
| level 6 | 35099 | 7.66 | 32.34 |
| level 8 | 28604 | 7.66 | 32.21 |
| **level 11 (default)** | **14028** | **7.66** | **31.84** |
| level 14 | 4179 | **8.07** | **28.11** |
| window 9x5 | 33227 | 7.66 | 32.34 |

Lower keeps more of a faint trace and more noise; higher gives a sparser but
cleaner one.

**The LUF and MUF these produce are not measurements.** Measured over a whole
real day — all 288 captures of 2026-02-04, cyprus1 → yoshkar-ola — LUF is 7.66
and MUF is 32.34 in almost every one: the two ends of the 7.5–32.5 MHz sweep.
276 of 288 report MUF ≥ 32.0, and there is no diurnal variation whatsoever, on
a circuit where `ionograms-handler`'s own archive of the same path shows the
MUF swinging from ~11 MHz at night to ~32 by 06 UTC.

Profiling surviving points per spectrum shows why. On a 14:15 capture whose
visible trace ends near 20 MHz:

| band | surviving points per spectrum |
|---|---|
| 8.5–10.6 MHz | 25–28 |
| 12.6–20.8 MHz | **3–7** ← where the trace actually is |
| 22.9–31.1 MHz | 12–31 |

The trace is a *minority* of what survives the gate. `usage_frequencies` asks
only for three consecutive spectra each holding three consecutive non-zero
points, which residual interference at the band edges satisfies trivially — and
because the speckle filter clusters what it keeps, it satisfies it more easily
than random noise would. So the numbers describe the interference environment,
not the ionosphere. Changing `obj_level` changes which noise survives, not
where the trace ends: the 31.84 → 28.11 shift in the table above is noise
moving, not a MUF measurement responding.

The picture is trustworthy; the two numbers derived from it are not. A real
extractor has to require delay continuity across spectra — the trace is a
connected curve, and that is the property the current test ignores.

**The threshold has cliffs, not a gradient.** The filter counts surviving
neighbours, so a trace occupying `k` delay rows inside the window contributes at
most `obj_size_horizontal · k`. Counts on a clean trace are therefore quantised
to multiples of the window width — measured 9 and 18 for a 9 x 3 window at one
and two rows thick — and what `obj_level` really selects is **how thick a trace
has to be to survive at all**:

| obj_level (9 x 3) | trace must be | effect |
|---|---|---|
| ≤ 9 | 1 row | keeps everything, background stays noisy |
| 10–18 | 2 rows | the default 11 and 14 both live here |
| 19–27 | 3 rows | only the core of a strong trace |
| > 27 | impossible | deletes the whole ionogram |

Above `obj_size_horizontal` a one-row-thick trace is deleted outright however
strong it is. Between the cliffs the threshold only changes how much *noise*
clumping survives: measured on a synthetic ionogram at the 38% density the
statistical gate passes, a clean two-row trace survives 100% at every level from
6 to 14 while noise falls from 96.9% to 14.3%.

Real traces are not uniformly two rows thick, which is why the measured table
above shows level 14 keeping 4179 points against 11's 14028 — the thin parts go
first, and the SNR and usage-frequency panels, computed from what survives,
thin out with them. The dialog states the required thickness rather than a
fraction of the window, since the fraction is not what the filter tests. Note what the band edges do: at level 11 the LUF/MUF span nearly
the whole sweep, which means noise is still being counted as signal. Stricter
cleaning narrows it to something physically plausible — worth remembering that
LUF and MUF are only as good as the gate that feeds them.

### Whitening — `whiten`, `whiten_len`, `whiten_n`

`juha::whiten(nfft, navg)` ran ahead of the downconvert and the archive was
recorded with it on (`whiten=1, whiten_len=8192, whiten_n=30000`). Its
implementation is **not** in the backup — only a stock GNU Radio header with no
description — so `Whitener` in `tools/rx_dechirp.py` is a reconstruction from
the parameter names and `chirp_calc.py`'s one-line description of it as "the
amplitude domain adaptive filter before chirp downconversion".

Each FFT bin is divided by the running RMS of that bin, so a strong broadcast
carrier contributes one bin at unit amplitude instead of a spike the dechirp
smears across the delay axis. Measured against a synthetic echo at 8.9 ms with
four strong carriers added, scoring the echo against the worst other peak:

| | echo above the rest |
|---|---|
| unwhitened | 27.7 dB |
| nfft 4096, navg 300 | 22.4 dB |
| nfft 8192, navg 3000 | 31.2 dB |
| **nfft 8192, navg 30000 (the archive's)** | **33.4 dB** |

The echo's beat frequency was -890.0 Hz in every case, so the delay it reports
is untouched. Two things this measurement settled:

- **Small `navg` makes it worse than not whitening at all.** The gain estimate
  wobbles from chunk to chunk and amplitude-modulates the trace. The archive's
  30000 averages over roughly ten seconds, which is why it was chosen.
- **It needs scipy.** numpy promotes complex64 to complex128 for every
  transform: 77 MS/s against scipy's 140 on the same machine. `make_whitener`
  refuses to start without it rather than quietly dropping samples.

Cost is the real constraint, and it is what forced the pipeline. Measured on
the sounding laptop with both stages in one thread:

| | throughput | real time |
|---|---|---|
| dechirp alone | 56.3 MS/s | 2.25x |
| + whitening, 1 FFT thread | 19.6 MS/s | 0.78x |
| + whitening, 2 FFT threads | 22.6 MS/s | 0.90x |

Under 1.0x, so it could not run. But sharing a thread makes the two costs add
when they need not: solving `1/(1/22.6 - 1/56.3)` puts whitening alone at about
38 MS/s, and both stages clear 25 individually. `capture_one` now runs the
whitener in its own thread feeding a bounded queue, so the rate is the slower
stage rather than the sum — `min(38, 56)` instead of 23. The queue is bounded
because an unbounded one would grow until the machine swapped if the dechirp
fell behind, which is a worse failure than dropping the capture. Backpressure
runs the existing path: the whitener blocks on the queue, `full_q` backs up,
the receive loop waits for a free buffer.

One further quarter of the cost came off the power estimate. Profiled per
1 MS block, whitening spends 32% in the forward transform, 32% in the inverse,
12% in the divide and **25% computing the mean power** — and that quarter buys
precision the filter cannot use. With `navg = 30000` the running average spans
about ten seconds, so beta is near 0.004 and the variance of any single update
is suppressed regardless of whether 122 rows or 30 went into it. The estimate
now reads every `rows // 32`'th row. Scored on the same synthetic echo, all
rows and every 32nd both give 33.4 dB, and so does every 8th; only the cold
start still pays full price, once per capture, because nothing is smoothing it
yet. Note that beta still tracks rows *received*, not rows sampled — the filter
should follow the channel at the rate the channel arrives.

`--benchmark` reports both arrangements, and the pipelined line is the one that
decides. **Run it on the sounding host before enabling whitening** — it buys
nothing if the result is a capture with holes in it, and receiving still needs
a core of its own on top of the two.

The threaded path was checked against the serial one for bit-identical output;
both stages are stateful, so the single consumer draining the queue in order is
load-bearing, not incidental.

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

### The variation panels' period — `snr_period_hour`, `pdp_period_hour`

Setting the period to 24 h drew only the last two hours. The panels trim
correctly; they were never given more than two hours to trim. `LoadLatestCaptures`
capped the archive replay at a fixed 24 captures, and at the standard 300 s
repetition 24 captures *is* exactly two hours — the coincidence is why it looked
like a working setting rather than a hard limit.

The cap is now `periodHour * 3600 / rep` per station, ceilinged at 600. The
ceiling matters because a short repetition and a long period multiply: 24 h at
60 s would be 1440 files, and on an archive whose sidecars have not been built
each one costs 80 MB and 610 FFTs.

Replaying history also used to re-render every capture into the control panel on
the way past, since `addIg` promotes the previous ionogram each time. Only the
last two are ever displayed, so `addIg(file, keepControl)` now skips that for
everything before them — half the work on a 288-file replay.

### The console orphaned its sounder

`QProcess: Destroyed while process is still running`, repeated once per console
exit, naming `sounder.sh`. There is one `QProcess`, parented to the main window,
and `~frmMain()` did nothing but `delete ui` — so Qt destroyed it during
teardown without stopping the child. The shell died; python kept the radio and
kept writing `.lfs` files.

That matters more than a stray process. Start the console again and two
sounders compete for one N210, which presents as overflows appearing from
nowhere on a host with 2.2x of dechirp headroom — and by the entry below, one
overflow cuts the rest of the sounding. The `STOP` button always stopped it
properly; only closing the window did not.

`closeEvent` now asks (a clean stop lets the sweep finish, up to 250 s, which is
too long to block a window close on) and the destructor kills unconditionally as
a backstop. `tools/host-setup/15-check-orphans.sh` finds any left over from
before.

### Real-time priority: the group is not the gate

`error in pthread_setschedparam` at startup means UHD could not raise its
receive thread priority, which is the usual cause of sporadic overflows. The
obvious reading — the user is not in group `usrp` — is not the whole story.
What UHD needs is `RLIMIT_RTPRIO` above zero, and **pam_limits grants that when
the session starts**. Being added to the group afterwards changes nothing until
a new login, and `newgrp` does not re-run pam_limits on most distributions. So
`getent group usrp` can name the user while the running session still has
`ulimit -Hr` of 0.

The station is reached over a remote desktop, and logging out to pick up a group
can end the only way in. `su - $USER` looked like a way to get a fresh PAM
session from inside the existing one, but it is not: it wipes `XAUTHORITY`, so
the console cannot reach the display, and on this host it did not raise
`ulimit -Hr` either.

The limit is a per-process resource limit, inherited by children, and root can
raise it on a running process. So the reliable route needs no new session at
all:

```
sudo prlimit --pid $$ --rtprio=99
ulimit -Hr                          # want 99
$HOME/.cache/ionozond-build/ionozond &
```

The console inherits it, and so does the sounder the console spawns. It lasts
as long as that shell; a reboot makes pam_limits apply it everywhere
permanently, which is worth doing once the remote desktop is known to come back
on its own. The check script reports `ulimit -Hr` rather than only the group,
since the limit is what actually decides.

### "D" is not the dechirp being slow

UHD prints `D` on stderr for a sequence gap: datagrams the kernel or the NIC
dropped before UHD ever saw them. `O` and stall time mean the host could not
consume fast enough; `D` means the samples never arrived. They call for
opposite fixes, and the sounder's own log shows both.

A capture with real-time priority correctly in place still lost 184.1 ms across
2 overflows, with two `D` markers in the same window — so the CPU was never the
constraint. 25 MS/s of sc16 is 100 MB/s, four fifths of a gigabit link.

The startup line says which regime the link is in:

```
[INFO] [USRP2] Current recv frame size: 1472 bytes
```

1472 is 1500 minus IP and UDP headers, so the interface MTU is still 1500 and
**jumbo frames never took**, whatever `SET_MTU=9000` reported. At 1472 bytes,
100 MB/s is about 68000 packets per second; at 9000 it is under 12500. That
factor of five is the difference between a link with margin and one without.

Measured on the station, the counters name the culprit exactly:

```
rx_missed_errors: 5809541
RX ring: 256 of 4096 available
net.core.rmem_max = 50000000
```

`rx_missed_errors` counts frames the NIC had nowhere to put because the driver
had not drained the ring — loss *below* UHD entirely, which no CPU headroom or
socket buffer can touch. The ring was sitting at the driver default of 256 out
of a possible 4096. That is the cause, and `ethtool -G eno1 rx 4096` is the fix;
`12-host-tuning.sh` now does it, though the setting does not survive a reboot.

Two things around it. `rmem_max` was 50 MB while the sounder asks UHD for
100 MB, and the kernel clamps silently, so the script now sets 100 MB.
And the MTU reads 9000 while UHD still reported `recv frame size: 1472` — its
probe fell back, and it has to be told: `--args recv_frame_size=8000`.

`open_radio` merges `recv_buff_size=100000000` and `num_recv_frames=4096` into
the device args unless the caller set them.
`tools/host-setup/16-check-packet-loss.sh` reports the MTU, the ring, the drop
counters and the ceiling together; run it during a capture and watch whether
the counters move.

### An overflow cuts the ionogram dead, and why

An overflow does not merely lose signal for its duration. The samples after it
arrive with no marker, so the dechirp mixes them against a replica for the
wrong instant. The replica sweeps at `rate`, so losing `d` seconds of samples
moves every subsequent echo by exactly `d` in apparent delay. Measured on a
synthetic echo at 8.9 ms:

| samples lost | beat after the gap | apparent delay |
|---|---|---|
| none | −890.0 Hz | 8.90 ms |
| 1 ms | −790.1 Hz | −1.00 ms (−300 km) |
| 3 ms | −590.0 Hz | −3.00 ms (−900 km) |
| 40 ms | +3110.1 Hz | −40.00 ms (−12000 km) |

The delay window is only a few ms wide — 8 to 13.5 ms on the Cyprus circuit,
with the trace at 9 ms, so barely 1 ms of margin below it. **One overflow of
more than about a millisecond pushes the trace out of the window for the whole
remainder of the sounding**, which looks exactly like an ionogram sharply cut at
whatever frequency the sweep had reached. The cut frequency then varies from
capture to capture, because overflows happen at random times — 17 MHz in one
sounding and 30 MHz five minutes later, which is not a rate any ionosphere
moves at.

UHD timestamps every buffer, so the true sample index is knowable.
`capture_one` now compares each buffer's timestamp against the count fed so far
and pads any shortfall with zeros before the samples that follow it. Padding
rather than re-basing the phase keeps the whitener, the decimator and the file
length aligned; the lost stretch goes blank instead of taking the rest of the
sounding with it. Zero-filling restored −890.0 Hz exactly at every gap size
above. `--no-gap-fill` returns the old behaviour.

The filler blocks travel the same queue as real buffers, so the queue carries a
recycle flag: a filler is a throwaway array of its own size, and returning it
to the pool would hand the receive loop a buffer too small for the next `recv`.

### Borrowed from ionograms-handler — the continuous ionogram

`ionograms-handler` renders an ionogram very differently, and the difference is
most of why its output reads as sharper off a comparable receiver. Its numbers
only mean anything together (`muf/spectro.py`, `muf/render.py`):

```python
NOISE_COEF = 4 * math.log(2)                      # spectro.py:31
floor = NOISE_COEF * np.median(spectrum)          # spectro.py:189, full spectrum
out[i] = row / floor                              # spectro.py:191, divide only
...
return 10.0 * np.log10(np.maximum(power, floor) / floor)   # to_db, floor=1e-3
...
DEFAULT_CMAP = "jet"; DEFAULT_VMIN_DB = 20.0; DEFAULT_VMAX_DB = 75.0
```

Referencing the log to `1e-3` rather than to 1 puts 0 dB thirty decibels below
unity, so the equalised noise floor lands near 26 dB — measured 26.1 on a
synthetic capture — and a window starting at 20 keeps a few dB of floor
visible. That is why the background reads as textured dark blue rather than
flat. Four things worth taking:

1. **Nothing is deleted.** They plot the continuous field and let the eye do
   the gating. Ours thresholds each spectrum, despeckles, and stores zeros for
   the rest — measured on the same synthetic capture, **the gate keeps 5.4% of
   the array and zeros the other 94.6%**. Multi-hop traces and the
   ordinary/extraordinary split live in exactly that discarded structure.
2. **A fixed colour window** rather than scaling to the peak. On a continuous
   image the peak is the direct signal and the structure worth seeing sits tens
   of dB below it.
3. **The log reference**, which is what makes 20 and 75 absolute numbers that
   mean the same thing in every capture instead of per-image percentages.
4. **jet**, added as `ig_colormap_index` 9.

`compute(..., iono_mode=...)` takes `"gated"` (the original, still the default)
or `"snr"`. **LUF, MUF, SNR and PDP are derived from the gated array in both
modes** — verified identical, 7.67 / 9.83 MHz either way — so switching changes
what is displayed and nothing that is measured.

Note the noise coefficient is theirs, not ours: `NOISE_FACTOR` here is 2·ln2 =
1.3863 and theirs is 4·ln2 = 2.7726, both commented as converting the median of
an exponential to its mean. That conversion is 1/ln2 = 1.4427, since the median
of an exponential is `mean·ln2`. Ours is 3.9% below it (−0.17 dB) — close by
coincidence, because 2·ln²2 = 0.961 — and theirs is exactly 2× ours, +2.84 dB
above the correct value.

Where the constant multiplies the median to form a **threshold**, the fraction
of pure-noise cells that survive is exactly `2^-F`:

| F | value | dB above median | noise cells passing |
|---|---|---|---|
| 2·ln2 (ours) | 1.3863 | 1.42 | 38.3% |
| 1/ln2 (median→mean) | 1.4427 | 1.59 | 36.8% (= 1/e) |
| 4·ln2 (theirs) | 2.7726 | 4.43 | 14.6% |

Which is right depends on the job. As a *divisor* — their use — it only sets
where the colour window sits, and theirs is calibrated against it, so
reproducing their scale means keeping 4·ln2. As a *threshold* — our use, in the
second gate — 2·ln2 passes 38% of noise, which is why the Rosin threshold and
the speckle filter have to do the real work downstream.

The `.lfp` header gained `min_value_db` at 0x120 and the mode flag at 0x124,
which were unused padding. Sidecars written before this read back as 0.0 and
gated, which is the old behaviour exactly. `RasterData` gained `setPowerMin`
because its Z interval started at a hard-coded 0.0.

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

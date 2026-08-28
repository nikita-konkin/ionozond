# acquisition

The sounder: the half that drives the radio and writes `.lfs` captures.

**Both sounders live in their own repositories.** This directory holds the
integration notes and is where they are referenced (submodule or checkout), not
where their code is vendored. That separation is deliberate — see *Licensing*
below.

## The two sounders

### chirpsounder2 — the main path

<https://github.com/nikita-konkin/chirpsounder2>, a fork of
<https://github.com/jvierine/chirpsounder2>. Python 3, `digital_rf`, no GNU
Radio out-of-tree module. **MIT licensed.**

What it needs from this project:

- an **`.lfs` writer**, so its captures are readable by the console and the
  existing archive tooling — see [`../docs/lfs-format.md`](../docs/lfs-format.md)
- optionally an **`.lfp` writer**, so derived products exist without a second
  pass over the archive — see [`../docs/lfp-format.md`](../docs/lfp-format.md)

Note it already carries a `web/` directory; worth checking what that does
before deciding how much the Qt console should overlap with it.

### chirpsounder1 — the low-resource alternative

Not yet created. A **port** of the original sounder rather than a frozen copy:
same algorithm and same small footprint, on current Python and libraries. Kept
because it runs where chirpsounder2 will not — a laptop at a remote site, an
older receiver machine.

Its ancestor is `gr-juha`: GNU Radio 3.7, Python 2, a C++ out-of-tree module.
Copies of the originals are in `reference/` at the top of this repository
(kept locally for cross-checking, not redistributed).

Porting notes:

- **Python 2 → 3.** `chirp.py`, `chirp_config.py`, `lfs_header.py`, `os_time.py`
  are the whole application layer.
- **Drop the OOT module for writing.** In `gr-juha` the `.lfs` writer lives
  inside the C++ `chirp_downconvert` block, which is the only reason the module
  is needed in order to record. In Python it is a `struct.pack` of the 512-byte
  header plus `.tofile()` on a `complex64` array.
- **Keep the dechirp in C or numpy, not GNU Radio,** if the low footprint is to
  survive. The dechirp is a complex multiply by a swept phasor followed by a
  decimating filter, and that is where the resource advantage actually lives.

## What the receiver hardware requires

From the first host brought up (see
[`../tools/host-setup/README.md`](../tools/host-setup/README.md) for the full
probe). Both sounders need these in their configuration, whichever is running:

| setting | value | why |
|---|---|---|
| `addr` | `192.168.10.3` | the radio is **not** on the factory `.2` |
| `subdev` | `A:A` | LFRX frontend `A`, a real input |
| `clock_source` / `time_source` | `gpsdo` | an internal FireFly GPSDO is fitted |
| `sample_rate` | `25e6` | matches the archive; 100 MB/s over the link |
| gain | *none available* | LFRX has no analog gain and no attenuator |

`subdev` is the one that fails silently. LFRX exposes `A`, `B`, `AB` and `BA`;
`AB` pairs RXA as I with RXB as Q, so with a single antenna it halves the
amplitude and mirrors the spectrum — a plausible-looking capture that is
quietly wrong. Set it explicitly rather than relying on UHD's default.

The 25 MS/s figure is the radio's ceiling on gigabit Ethernet, and the
decimation by 625 happens on the host because the dechirp has to see the whole
swept band. This is exactly the load that motivates a low-footprint
chirpsounder1 — on the 2011-vintage laptop at this site, the dechirp is the
budget.

## ⚠ Licensing: the two generations are not compatible

| Component | Licence |
|---|---|
| `jvierine/chirpsounder2` and forks of it | **MIT** |
| `gr-juha` — the v1 sounder | **GPL-3.0** |
| this project (console, `.lfp`, tooling) | ours to choose |

A `chirpsounder1` that is a port of `gr-juha` inherits **GPL-3.0**. It cannot
be MIT like its sibling, and it cannot be folded into this repository without
raising the question of whether the console becomes a derivative work.

This is the reason the sounders stay in their own repositories:

```
nikita-konkin/chirpsounder2    MIT       fork of upstream, the main path
nikita-konkin/chirpsounder1    GPL-3.0   port of gr-juha, the light path
nikita-konkin/ionozond         ours      console, formats, tooling
```

The console talks to both through the file formats only — it writes
`chirp_config.py` and reads `.lfs`. That is an interface, not a dependency, so
nothing here needs to inherit either licence.

## Must hold

Captures from chirpsounder1 and chirpsounder2 must be byte-compatible, so an
archive stays readable whichever recorded it.

Settle the **coordinate encoding** too. The operator types station positions
into `chirp_config.py` and both sounders pack them into the header verbatim:

```python
rx_station = {'name':'yoshkar-ola','lat':56.38,'lon':47.53}
```

Those are degrees and minutes — 56°38′ N, 47°53′ E — while every reader treats
them as decimal degrees. A GPS fix at that site confirms it: the `DD.MM`
reading lands 1.3 km away, the decimal reading 34.6 km. See
[`../docs/lfs-format.md`](../docs/lfs-format.md). Whichever convention wins,
both sounders must use it, and the field's meaning must be stated where the
operator enters it rather than left to be guessed.

Settle the header version split before either becomes the standard producer:
the C++ writer emits `format_ver 1.0 / header_size 498`, the Python writer
emits `1.1 / 512`, and they disagree about what `header_size` even counts. The
console accepts only the former. See
[`../docs/lfs-format.md`](../docs/lfs-format.md).

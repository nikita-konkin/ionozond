# LFS — the capture format

A `.lfs` file is a **512-byte packed header** followed by a raw stream of
interleaved float32 I/Q samples (`complex64`) at `if_rate = sample_rate / dec`.

```
cyprus1_20191023_071510.lfs   =   512 bytes header
                                + 10,000,000 x 8 bytes  (250 s at 40 kHz)
                                = 80,000,512 bytes
```

Naming: `<station>_<yyyyMMdd>_<hhmmss>.lfs`, stored under
`<archive>/<yyyy.MM.dd>/`.

## Header

Byte offsets, little-endian, `#pragma pack(1)`. Verified field by field against
real captures.

| Off | Type | Field | Example |
|---|---|---|---|
| 0x000 | `char[4]` | `format` | `LFSG` |
| 0x004 | `float` | `format_ver` | 1.0 |
| 0x008 | `char[4]` | `header_id` | `fmt ` |
| 0x00C | `uint16` | `header_size` | 498 — see the warning below |
| 0x00E | `char[64]` | `tx_name` | `cyprus1` |
| 0x04E | `float` | `tx_latitude` | 35.0 |
| 0x052 | `float` | `tx_longitude` | 34.0 |
| 0x056 | `char[64]` | `rx_name` | `yoshkar-ola` |
| 0x096 | `float` | `rx_latitude` | 56.38 |
| 0x09A | `float` | `rx_longitude` | 47.53 |
| 0x09E | `uint16` | `start_year` | 2019 |
| 0x0A0 | `uint16` | `start_daynumber` | 296 |
| 0x0A2 | `uint16` | `start_month` | 10 |
| 0x0A4 | `uint16` | `start_day` | 23 |
| 0x0A6 | `uint16` | `start_hour` | 7 |
| 0x0A8 | `uint16` | `start_minute` | 15 |
| 0x0AA | `uint16` | `start_second` | 10 |
| 0x0AC | `uint32` | `start_epoch` | 1571814910 |
| 0x0B0 | `uint32` | `chirpt` | 10 — offset into the repetition period, s |
| 0x0B4 | `uint32` | `cf` | 20000000 — centre frequency, Hz |
| 0x0B8 | `uint16` | `dur` | 250 — sounding length, s |
| 0x0BA | `uint32` | `rate` | 100000 — chirp rate, Hz/s |
| 0x0BE | `uint32` | `rep` | 300 — repetition period, s |
| 0x0C2 | `int32` | `rmin` | 0 |
| 0x0C6 | `int32` | `rmax` | 5000 |
| 0x0CA | `uint32` | `dec` | 625 |
| 0x0CE | `uint32` | `sample_rate` | 25000000 — Hz |
| 0x0D2 | `uint16` | `whiten` | 0 |
| 0x0D4 | `uint32` | `whiten_len` | 8192 |
| 0x0D8 | `uint32` | `whiten_n` | 20000 |
| 0x0DC | `char[292]` | `reserved` | zero |

`start_epoch` agrees with the broken-down fields; either may be used.

## ⚠ Coordinates are degrees and minutes, and everything reads them as degrees

`rx_latitude = 56.38`, `rx_longitude = 47.53` are **not** decimal degrees. They
are degrees and minutes written as `DD.MM`: 56°38′ N, 47°53′ E. Yoshkar-Ola,
which is what `rx_name` says, sits at 56.6388 N / 47.8908 E — that is
56°38.3′ N, 47°53.4′ E. The header value is the city's position to the nearest
minute.

Two independent checks, on the path `cyprus1` → `yoshkar-ola`:

| reading | position | distance from a GPS fix taken at the receiving site |
|---|---|---|
| decimal degrees | 56.3800 N, 47.5300 E | **34.6 km** |
| `DD.MM` | 56.6333 N, 47.8833 E | **1.3 km** |

The GPS fix came from the FireFly GPSDO in the site's own N210
(`$GPGGA,...,5637.3262,N,04753.1006,E,...`), so it is a direct measurement of
where the receiver actually stands.

The second check is the data. `earthDistanceKm` feeds `rayDistanceKm`, which
assumes reflection at 100 km, so the leading edge of a one-hop echo should sit
a little *above* the ray distance:

| reading | ray distance | measured leading edge at 2655 km is |
|---|---|---|
| decimal degrees | 2599.0 km | +56.0 km above |
| `DD.MM` | 2634.1 km | **+20.9 km above** |

Nothing converts. The operator types the numbers into the sounder's
`chirp_config.py`:

```python
rx_station = {'name':'yoshkar-ola','lat':56.38,'lon':47.53}
```

and they are packed into the header verbatim. Every reader downstream —
`earthDistanceKm` here, `earth_distance` in the operators' own `distance.py` —
then multiplies by `pi/180` as though they were decimal degrees.

**The consequence is contained.** The distance only positions the delay-axis
window, which is 1680 km wide, so a 35 km error does not hide the echo. It does
mean the window is placed slightly nearer than the geometry warrants, and any
virtual-height figure derived from the ground distance inherits the error.

**This is not fixed in the reconstruction.** `earthDistanceKm` reproduces the
original, and changing it would silently shift every existing display. The
decision — reinterpret the archive, or normalise new captures to decimal
degrees — belongs to whoever owns the archive. What `.lfp` should do is store
decimal degrees explicitly and convert on the way in, so the ambiguity stops
at the boundary.

Caveats: the transmitter coordinates in this archive are whole degrees
(`35`, `34`), so they cannot distinguish the two readings, and only one
receiving station has been checked against a GPS fix.

## ⚠ The version and `header_size` split

Three definitions exist in the wild and they do not agree:

| Producer | `format_ver` | `header_size` | `reserved` |
|---|---|---|---|
| C++ writer in `gr-juha` — produced the existing archive | 1.0 | **498** | 292 |
| `chirpsounder/lfs_header.py` | **1.1** | **512** | 306 |
| what the console accepts | only 1.0 | only 498 | |

The C++ side stores `header_size` as *the bytes after the 14-byte preamble*
(512 − 14 = 498); the Python side stores *the whole struct* (512). And the
console compares `format_ver` to `1.0f` for **exact float equality**, so a
capture written by the Python path is silently rejected.

If you are standardising on `.lfs`, resolve this first: one meaning for
`header_size`, one version both writers emit, and readers that fail loudly.
The sidecar format deliberately avoids all three mistakes — see
[`lfp-format.md`](lfp-format.md).

## Reading it

```python
import numpy as np
LFS_HEADER_SIZE = 512

def read_lfs(path):
    with open(path, 'rb') as f:
        header = f.read(LFS_HEADER_SIZE)
        assert header[:4] == b'LFSG'
        iq = np.fromfile(f, dtype=np.complex64)
    return header, iq
```

## From samples to an ionogram

Each block of `fft_count` samples becomes one spectrum, i.e. one column:

```
z[k]     = w[k] * (I[k] + j*Q[k])     periodic Hanning, 0.5*(1-cos(2*pi*k/n))
S        = fft(z)
power[k] = |S[k]|^2
power    = fftshift(power)
power   /= median(power)               noise-floor normalisation
power    = power[::-1]                 delay axis flip
```

The final reversal matters. Dechirping against a local replica that runs ahead
of the received sweep drives the beat frequency negative as echo delay grows,
so after `fftshift` the rows run long-delay-first; reversing puts them back in
ascending-delay order. Dropping it puts the returned signal at a negative
height.

Axes:

- frequency: `cf - sample_rate/2` up to `+ dur * rate`
- delay: `±(if_rate/2)/rate` as a light path, i.e. `±3e8·(if_rate/2)/rate` metres
- the displayed window is the great-circle ray path ±180 km, 1500 km tall

See `docs/reverse-engineering.md` for how each of these was established.

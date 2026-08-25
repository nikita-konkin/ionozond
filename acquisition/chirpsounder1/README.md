# chirpsounder1

The original chirp sounder, **ported** rather than frozen.

Kept because it is substantially lighter on CPU and memory than chirpsounder2
and runs where the newer one will not: a laptop at a remote site, an older
receiver machine. The goal is the same algorithm and the same small footprint,
on current Python and current libraries.

## Where it comes from

`gr-juha` — GNU Radio 3.7, Python 2, a C++ out-of-tree module. Copies of the
originals are in `reference/` at the top of this repository (third-party,
GPL-3.0, not redistributed).

## Porting notes

- **Python 2 → 3.** `chirp.py`, `chirp_config.py`, `lfs_header.py`,
  `os_time.py` and friends are the whole application layer.
- **Drop the OOT module for writing.** In `gr-juha` the `.lfs` writer lives
  inside the C++ `chirp_downconvert` block, which is the only reason the module
  is needed to record. In Python it is a `struct.pack` of the 512-byte header
  plus `.tofile()` on a `complex64` array.
- **Keep the dechirp in C or numpy, not GNU Radio,** if the low footprint is to
  survive. The dechirp is a complex multiply by a swept phasor followed by a
  decimating filter — that is where the resource advantage lives.
- **Emit `.lfp` alongside `.lfs`** so the archive is browsable without a
  second pass. See `docs/lfp-format.md`.

## Must hold

Captures from chirpsounder1 and chirpsounder2 must be byte-compatible, so an
archive stays readable whichever recorded it. Settle the header version split
first — see `../README.md`.

# acquisition

The sounder: the half that drives the radio and writes `.lfs` captures.

This directory is where chirpsounder belongs. It is empty in the working tree
because the code currently lives elsewhere — bringing it in is the next step,
and there are two paths to carry:

## Two sounders, on purpose

**`chirpsounder2/`** — the current one. Python 3, `digital_rf`, no GNU Radio
out-of-tree module. This is the main acquisition path.

**`chirpsounder1/`** — the original, kept deliberately because it is
substantially lighter on CPU and memory and runs where the newer one will not:
a laptop at a remote site, an older receiver machine. The intent is to *port*
it to current Python and current libraries rather than freeze it — same
algorithm, same low footprint, maintainable.

Both must produce the same `.lfs` files, so an archive stays readable whichever
recorded it.

## Writing `.lfs`

In the original `gr-juha`, the writer lived inside the C++ `chirp_downconvert`
GNU Radio block — which is the only reason the OOT module was needed at all.
In Python it is a `struct.pack` of the 512-byte header followed by
`.tofile()` on a `complex64` array: roughly 40 lines, no compiled dependency.

See [`../docs/lfs-format.md`](../docs/lfs-format.md) for the layout.

### Settle the version split first

There are currently **three inconsistent definitions** of the header in the
existing toolchain:

| Definition | `format_ver` | `header_size` |
|---|---|---|
| C++ writer (`gr-juha`) — produced the whole existing archive | 1.0 | 498 |
| Python writer (`chirpsounder/lfs_header.py`) | 1.1 | 512 |
| the console | accepts **only** 1.0 / 498 | |

The two writers disagree about the version *and* about what `header_size`
counts: the C++ side stores the bytes after the 14-byte preamble, the Python
side stores the whole struct. Anything the Python writer produces is silently
refused by the console.

Before either sounder is made the standard producer:

1. pick one meaning for `header_size` and document it
2. agree a version number both sides emit
3. make readers **fail loudly** on an unknown version rather than mis-parse

The sidecar format was designed to avoid repeating this — see
[`../docs/lfp-format.md`](../docs/lfp-format.md), which uses two integer
version fields with a stated compatibility rule.

## Writing `.lfp` at acquisition time

Better still, have the sounder emit the derived-products sidecar as it records,
so no separate pass over the archive is ever needed. The console already reads
sidecars in preference to captures; `python/lfp.py` shows the structure and
`src/lfpfile.cpp` is the reference implementation.

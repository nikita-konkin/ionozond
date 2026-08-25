# LFP — LFS derived products sidecar

A small file written next to each `.lfs` capture holding everything needed to
*display and analyse* the sounding, so the 80 MB raw capture only has to be
read once.

```
cyprus1_20191023_071510.lfs     80,000,512 bytes   raw complex64
cyprus1_20191023_071510.lfp        ~90,000 bytes   products
```

The raw capture stays authoritative and can move to cold storage; the sidecar
is what the viewer, the archive browser and the trend plots actually read.

## Design rules, learned from `.lfs`

The `.lfs` header has three problems this format deliberately avoids:

1. **The version is a `float` compared for exact equality.** `dsChirp` rejects a
   capture unless `format_ver == 1.0f`. Here the version is two `uint16`s,
   major and minor, with a stated compatibility rule.
2. **`header_size` means different things to different writers** — the C++
   writer stores 498 (bytes after the 14-byte preamble), the Python writer
   stores 512 (the whole struct). Here `header_size` is **always the total
   size of the header in bytes, including the magic**, and there is only one
   writer definition.
3. **Fields the pipeline needs live outside the file.** `tb`,
   `lfsr_polynome_degree`, the FFT size and the gate thresholds all affect the
   result but are not recorded with it. Here every parameter that changes the
   output is stored in the file.

**Compatibility rule:** a reader must accept any file whose `version_major`
matches what it knows, ignoring sections it does not recognise, and must
refuse a file whose `version_major` is higher. Minor versions only ever add
sections or append fields inside the reserved area.

## Layout

```
offset  size   field
------  ----   -----------------------------------------------------------
0x000      4   char     magic[4]            "LFPR"
0x004      2   uint16   version_major       1
0x006      2   uint16   version_minor       0
0x008      4   uint32   header_size         512, always the whole header
0x00C      4   uint32   section_count       number of entries in the table
0x010      4   uint32   section_table_off   byte offset of the section table
0x014      4   uint32   flags               bit 0: source capture was gated
0x018      8   char     producer[8]         "dsChirp " / "chirp2  " / ...
0x020     16   char     producer_version[16]

--- identity of the source capture, copied from its lfs_header ---
0x030     64   char     tx_name[64]
0x070      4   float    tx_latitude
0x074      4   float    tx_longitude
0x078     64   char     rx_name[64]
0x0B8      4   float    rx_latitude
0x0BC      4   float    rx_longitude
0x0C0      8   int64    start_epoch_ms      UTC, milliseconds
0x0C8      4   uint32   cf_hz
0x0CC      4   uint32   rate_hz_s
0x0D0      4   uint32   sample_rate_hz
0x0D4      4   uint32   dec
0x0D8      2   uint16   dur_s
0x0DA      2   uint16   whiten
0x0DC      4   uint32   whiten_len
0x0E0      4   uint32   whiten_n

--- how the products were computed ---
0x0E4      4   uint32   fft_count           points per spectrum
0x0E8      4   uint32   spec_count          spectra in the capture
0x0EC      4   uint32   spec_point_count    delay rows kept (the window)
0x0F0      4   float    freq_min_mhz        displayed sweep
0x0F4      4   float    freq_max_mhz
0x0F8      4   float    delay_min_ms        displayed delay window
0x0FC      4   float    delay_max_ms
0x100      4   float    noise_gate_db       Rosin threshold actually applied
0x104      4   float    max_value_db        frame maximum before gating
0x108      4   float    luf_mhz             -1 when not determined
0x10C      4   float    muf_mhz             -1 when not determined
0x110      4   int32    luf_index
0x114      4   int32    muf_index
0x118      4   uint32   tb                  from config.ini, 0 if unused
0x11C      4   uint32   lfsr_polynome_degree

0x120    224   char     reserved[224]       zero-filled
0x200          section table starts here
```

All integers little-endian. Strings are NUL-padded and need not be
NUL-terminated when they fill the field.

## Section table

`section_count` entries of 32 bytes each:

```
offset  size   field
------  ----   -----------------------------------------------------------
0x00       4   char     type[4]        see below
0x04       2   uint16   dtype          1 = float32, 2 = int32, 3 = uint8
0x06       2   uint16   compression    0 = none, 1 = raw zlib (RFC 1950)
0x08       4   uint32   rows
0x0C       4   uint32   cols
0x10       8   uint64   offset         byte offset of the payload
0x18       8   uint64   length         payload length ON DISK (compressed)
```

`rows * cols * sizeof(dtype)` is the length after decompression.

### Section types

| type | shape | contents |
|---|---|---|
| `IONO` | `spec_count` × `spec_point_count`, float32 | the gated ionogram in dB, row-major by spectrum, delay ascending |
| `SNR ` | 1 × `spec_count`, float32 | per-spectrum signal/noise in dB |
| `PDP ` | 1 × `spec_point_count`, float32 | integrated power per delay bin |
| `MASK` | `spec_count` × `spec_point_count`, uint8 | optional: 1 where a point survived the gate |
| `TRAC` | n × 2, float32 | optional: extracted trace, (MHz, ms) pairs |

Readers must skip unknown types. `IONO` compresses extremely well because
gating zeroes most of it — typically 20–40× on real captures.

## Reading it in Python

```python
import numpy as np, struct, zlib

def read_lfp(path):
    with open(path, 'rb') as f:
        head = f.read(512)
        assert head[:4] == b'LFPR'
        major, minor, hsize, nsec, tab_off = struct.unpack_from('<HHIII', head, 4)
        assert major == 1, f'unsupported major version {major}'

        f.seek(tab_off)
        table = f.read(nsec * 32)

        out = {}
        dt = {1: np.float32, 2: np.int32, 3: np.uint8}
        for i in range(nsec):
            typ, dtype, comp, rows, cols, off, length = \
                struct.unpack_from('<4sHHIIQQ', table, i * 32)
            f.seek(off)
            raw = f.read(length)
            if comp == 1:
                raw = zlib.decompress(raw)
            arr = np.frombuffer(raw, dtype=dt[dtype])
            out[typ.decode().strip()] = arr.reshape(rows, cols)
        return head, out
```

## Why not HDF5 / npz

`digital_rf` already gives chirpsounder2 an HDF5 dependency, so HDF5 was the
obvious candidate. It was rejected because the *viewer* is C++/Qt and has no
HDF5 dependency today; adding `libhdf5` to a Qt desktop build to read a
90 KB sidecar is a poor trade. `npz` has the mirror problem in the other
direction. A fixed header plus a section table is ~30 lines of numpy and
~80 lines of C++, needs nothing beyond zlib (which Qt already links), and
keeps the same shape as `.lfs` so the toolchain stays coherent.

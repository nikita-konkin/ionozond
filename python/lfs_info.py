#!/usr/bin/env python3
"""Print an .lfs capture's header, and the filename the console expects.

    python3 lfs_info.py capture.lfs
    python3 lfs_info.py '/mnt/maxtor/$RECYCLE.BIN/.../$RFZ8RIC.lfs'

Useful when a capture has lost its name -- recovered from a recycle bin, say --
because the header carries the station and the timestamp, which is everything
the canonical name is built from.

Header layout: docs/lfs-format.md. Offsets are byte-exact and packed.
"""

import os
import struct
import sys


def read_header(path):
    with open(path, "rb") as fh:
        raw = fh.read(512)
    if len(raw) < 512:
        raise ValueError("shorter than a 512-byte header (%d bytes)" % len(raw))

    def s(off, n):
        return raw[off:off + n].split(b"\0")[0].decode("latin-1").strip()

    def u16(off):
        return struct.unpack_from("<H", raw, off)[0]

    def u32(off):
        return struct.unpack_from("<I", raw, off)[0]

    def i32(off):
        return struct.unpack_from("<i", raw, off)[0]

    def f32(off):
        return struct.unpack_from("<f", raw, off)[0]

    h = {
        "format": s(0x000, 4),
        "format_ver": f32(0x004),
        "header_id": s(0x008, 4),
        "header_size": u16(0x00C),
        "tx_name": s(0x00E, 64),
        "tx_latitude": f32(0x04E),
        "tx_longitude": f32(0x052),
        "rx_name": s(0x056, 64),
        "rx_latitude": f32(0x096),
        "rx_longitude": f32(0x09A),
        "start_year": u16(0x09E),
        "start_daynumber": u16(0x0A0),
        "start_month": u16(0x0A2),
        "start_day": u16(0x0A4),
        "start_hour": u16(0x0A6),
        "start_minute": u16(0x0A8),
        "start_second": u16(0x0AA),
        "start_epoch": u32(0x0AC),
        "chirpt": u32(0x0B0),
        "cf": u32(0x0B4),
        "dur": u16(0x0B8),
        "rate": u32(0x0BA),
        "rep": u32(0x0BE),
        "rmin": i32(0x0C2),
        "rmax": i32(0x0C6),
        "dec": u32(0x0CA),
        "sample_rate": u32(0x0CE),
        "whiten": u16(0x0D2),
        "whiten_len": u32(0x0D4),
        "whiten_n": u32(0x0D8),
    }
    return h


def ddmm(value):
    """Show both readings of a coordinate. The archive stores DD.MM (degrees and
    minutes); every reader in the chain treats it as decimal degrees. See the
    warning in docs/lfs-format.md."""
    deg = int(value)
    minutes = round((value - deg) * 100)      # .MM is minutes, not a fraction
    return deg + minutes / 60.0


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]

    try:
        h = read_header(path)
    except (OSError, ValueError) as exc:
        print("cannot read %s: %s" % (path, exc))
        return 1

    if h["format"] != "LFSG":
        print("not an .lfs capture: magic is %r, expected 'LFSG'" % h["format"])
        return 1

    size = os.path.getsize(path)
    if_rate = h["sample_rate"] / h["dec"] if h["dec"] else 0
    samples = (size - 512) / 8.0                      # complex64
    expected = if_rate * h["dur"]

    print("file        %s" % path)
    print("            %d bytes" % size)
    print("format      %s v%.1f, header_id %r, header_size %d"
          % (h["format"], h["format_ver"], h["header_id"], h["header_size"]))
    if h["format_ver"] != 1.0 or h["header_size"] != 498:
        print("            *** the console accepts only v1.0 / 498;"
              " see docs/lfs-format.md")
    print()
    print("transmitter %s" % h["tx_name"])
    print("            %.4f / %.4f as stored"
          % (h["tx_latitude"], h["tx_longitude"]))
    print("            %.4f / %.4f if read as degrees+minutes"
          % (ddmm(h["tx_latitude"]), ddmm(h["tx_longitude"])))
    print("receiver    %s" % h["rx_name"])
    print("            %.4f / %.4f as stored"
          % (h["rx_latitude"], h["rx_longitude"]))
    print("            %.4f / %.4f if read as degrees+minutes"
          % (ddmm(h["rx_latitude"]), ddmm(h["rx_longitude"])))
    print()
    print("start       %04d-%02d-%02d %02d:%02d:%02d UTC  (day %d, epoch %d)"
          % (h["start_year"], h["start_month"], h["start_day"],
             h["start_hour"], h["start_minute"], h["start_second"],
             h["start_daynumber"], h["start_epoch"]))
    print("sounding    cf %.3f MHz, rate %g kHz/s, dur %d s, rep %d s, chirpt %d s"
          % (h["cf"] / 1e6, h["rate"] / 1e3, h["dur"], h["rep"], h["chirpt"]))
    print("sampling    %d Hz / dec %d = %g Hz  (range %d..%d km)"
          % (h["sample_rate"], h["dec"], if_rate, h["rmin"], h["rmax"]))
    if h["whiten"]:
        print("whitening   on, len %d, n %d" % (h["whiten_len"], h["whiten_n"]))
    print()
    print("samples     %.0f present, %.0f expected from dur x if_rate"
          % (samples, expected))
    # Complete captures overshoot slightly -- the writer flushes whatever is in
    # the last buffer, so a handful of extra samples is normal and not a fault.
    # Only a genuine shortfall is worth reporting.
    if expected:
        if samples < expected - 1:
            print("            *** %.1f%% of a full capture -- truncated or"
                  " still being written" % (100.0 * samples / expected))
        elif samples > expected * 1.001:
            print("            (%.0f more than expected -- unusually long)"
                  % (samples - expected))

    name = "%s_%04d%02d%02d_%02d%02d%02d.lfs" % (
        h["tx_name"], h["start_year"], h["start_month"], h["start_day"],
        h["start_hour"], h["start_minute"], h["start_second"])
    print()
    print("canonical name for this capture:")
    print("    %s" % name)
    print("the console groups captures by day, so it belongs at:")
    print("    <archive>/%04d.%02d.%02d/%s"
          % (h["start_year"], h["start_month"], h["start_day"], name))
    return 0


if __name__ == "__main__":
    sys.exit(main())

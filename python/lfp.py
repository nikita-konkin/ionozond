#!/usr/bin/env python3
"""
Reader for the LFP derived-products sidecar. See docs/lfp-format.md.

    from lfp import read_lfp
    meta, data = read_lfp('cyprus1_20191023_071510.lfp')
    iono = data['IONO']          # (spec_count, spec_point_count) float32, dB
    snr  = data['SNR'][0]        # (spec_count,)  float32, dB
    pdp  = data['PDP'][0]        # (spec_point_count,) float32

Standalone:
    python3 lfp.py file.lfp            # summary
    python3 lfp.py file.lfp out.png    # render the ionogram
"""
import struct
import sys
import zlib

import numpy as np

MAGIC = b'LFPR'
VERSION_MAJOR = 1
HEADER_SIZE = 512
SECTION_ENTRY = 32

_DTYPE = {1: np.float32, 2: np.int32, 3: np.uint8}


def _text(raw):
    return raw.split(b'\0', 1)[0].decode('utf-8', 'replace')


def read_lfp(path):
    """Returns (meta dict, {section name: 2-D ndarray})."""
    with open(path, 'rb') as f:
        head = f.read(HEADER_SIZE)
        if len(head) != HEADER_SIZE or head[:4] != MAGIC:
            raise ValueError('%s is not an LFP file' % path)

        major, minor, hsize, nsec, tab_off, flags = \
            struct.unpack_from('<HHIIII', head, 4)
        if major != VERSION_MAJOR:
            raise ValueError('unsupported LFP major version %d (this reader '
                             'understands %d)' % (major, VERSION_MAJOR))

        meta = {
            'version': (major, minor),
            'gated': bool(flags & 1),
            'producer': _text(head[0x18:0x20]),
            'producer_version': _text(head[0x20:0x30]),
            'tx_name': _text(head[0x30:0x70]),
            'tx_lat': struct.unpack_from('<f', head, 0x70)[0],
            'tx_lon': struct.unpack_from('<f', head, 0x74)[0],
            'rx_name': _text(head[0x78:0xB8]),
            'rx_lat': struct.unpack_from('<f', head, 0xB8)[0],
            'rx_lon': struct.unpack_from('<f', head, 0xBC)[0],
            'start_epoch_ms': struct.unpack_from('<q', head, 0xC0)[0],
            'cf_hz': struct.unpack_from('<I', head, 0xC8)[0],
            'rate_hz_s': struct.unpack_from('<I', head, 0xCC)[0],
            'sample_rate_hz': struct.unpack_from('<I', head, 0xD0)[0],
            'dec': struct.unpack_from('<I', head, 0xD4)[0],
            'dur_s': struct.unpack_from('<H', head, 0xD8)[0],
            'fft_count': struct.unpack_from('<I', head, 0xE4)[0],
            'spec_count': struct.unpack_from('<I', head, 0xE8)[0],
            'spec_point_count': struct.unpack_from('<I', head, 0xEC)[0],
            'freq_min_mhz': struct.unpack_from('<f', head, 0xF0)[0],
            'freq_max_mhz': struct.unpack_from('<f', head, 0xF4)[0],
            'delay_min_ms': struct.unpack_from('<f', head, 0xF8)[0],
            'delay_max_ms': struct.unpack_from('<f', head, 0xFC)[0],
            'noise_gate_db': struct.unpack_from('<f', head, 0x100)[0],
            'max_value_db': struct.unpack_from('<f', head, 0x104)[0],
            'luf_mhz': struct.unpack_from('<f', head, 0x108)[0],
            'muf_mhz': struct.unpack_from('<f', head, 0x10C)[0],
            'luf_index': struct.unpack_from('<i', head, 0x110)[0],
            'muf_index': struct.unpack_from('<i', head, 0x114)[0],
        }

        f.seek(tab_off)
        table = f.read(nsec * SECTION_ENTRY)

        out = {}
        for i in range(nsec):
            typ, dtype, comp, rows, cols, off, length = \
                struct.unpack_from('<4sHHIIQQ', table, i * SECTION_ENTRY)
            if dtype not in _DTYPE:
                continue                      # skip what we do not know
            f.seek(off)
            raw = f.read(length)
            if comp == 1:
                raw = zlib.decompress(raw)
            arr = np.frombuffer(raw, dtype=_DTYPE[dtype])
            out[typ.decode('ascii').strip()] = arr.reshape(rows, cols)

    return meta, out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    meta, data = read_lfp(sys.argv[1])

    import datetime
    when = datetime.datetime.utcfromtimestamp(meta['start_epoch_ms'] / 1000.0)
    print('%s -> %s   %s UTC' % (meta['tx_name'], meta['rx_name'],
                                 when.strftime('%Y-%m-%d %H:%M:%S')))
    print('  produced by %s %s, LFP v%d.%d, gated=%s'
          % (meta['producer'], meta['producer_version'],
             meta['version'][0], meta['version'][1], meta['gated']))
    print('  freq   %.3f .. %.3f MHz' % (meta['freq_min_mhz'], meta['freq_max_mhz']))
    print('  delay  %.3f .. %.3f ms' % (meta['delay_min_ms'], meta['delay_max_ms']))
    print('  LUF %.3f MHz   MUF %.3f MHz' % (meta['luf_mhz'], meta['muf_mhz']))
    for name, arr in sorted(data.items()):
        print('  %-5s %-14s %8.1f kB in memory'
              % (name, str(arr.shape), arr.nbytes / 1024.0))

    if len(sys.argv) > 2:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt

        iono = data['IONO']
        plt.figure(figsize=(9, 5))
        plt.imshow(iono.T, aspect='auto', origin='lower', cmap='jet',
                   extent=[meta['freq_min_mhz'], meta['freq_max_mhz'],
                           meta['delay_min_ms'], meta['delay_max_ms']],
                   vmin=0)
        plt.colorbar(label='dB')
        plt.xlabel('f, MHz')
        plt.ylabel('t, ms')
        plt.title('%s %s' % (meta['tx_name'], when.strftime('%Y-%m-%d %H:%M:%S')))
        plt.tight_layout()
        plt.savefig(sys.argv[2], dpi=110)
        print('  wrote %s' % sys.argv[2])
    return 0


if __name__ == '__main__':
    sys.exit(main())

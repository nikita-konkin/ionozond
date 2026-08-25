#!/usr/bin/env python3
"""
Independent NumPy implementation of the dsChirp spectrum pipeline, used to
check the reconstructed C++ against something that shares none of its code.

The pipeline, as decoded from QRxIonogram::readNewLfsSpecData @0x46e6f0:

    z[k]     = w[k] * (I[k] + 1j*Q[k])      periodic Hanning, 0.5*(1-cos(2*pi*k/n))
    S        = fft(z)
    power[k] = |S[k]|**2
    power    = fftshift(power)
    power   /= median(power)                 see note on getMedian below
    power    = power[::-1]

getMedian(): the original always averages the element at n/2 (after
nth_element) with the max of the lower half, with no odd/even branch. For even
n that is numpy.median; n is a power of two here, so numpy.median matches.

  spectrum_oracle.py <file.lfs> <specPointCount> <nSpec> [cpp_dump.bin]
"""
import sys
import numpy as np

LFS_HEADER_SIZE = 512


def hanning_periodic(n):
    """0.5*(1-cos(2*pi*k/n)), with the step narrowed to float32 as the original does."""
    step = np.float32(2.0 * np.pi / n)
    k = np.arange(n, dtype=np.float32)
    return (0.5 * (1.0 - np.cos((k * step).astype(np.float64)))).astype(np.float32)


def build_spectra(path, spec_point_count, n_spec):
    with open(path, 'rb') as f:
        f.seek(LFS_HEADER_SIZE)
        need = spec_point_count * n_spec
        data = np.fromfile(f, dtype=np.complex64, count=need)

    got = len(data) // spec_point_count
    data = data[:got * spec_point_count].reshape(got, spec_point_count)

    w = hanning_periodic(spec_point_count)
    z = data * w                      # complex64 * float32
    z = z.astype(np.complex128)       # the original converts to double before FFT

    spec = np.fft.fft(z, axis=1)
    power = (spec.real ** 2 + spec.imag ** 2)
    power = np.fft.fftshift(power, axes=1)
    med = np.median(power, axis=1, keepdims=True)
    power = power / med
    power = power[:, ::-1]
    return power, w


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path = sys.argv[1]
    n = int(sys.argv[2])
    n_spec = int(sys.argv[3])
    cpp_dump = sys.argv[4] if len(sys.argv) > 4 else None

    power, w = build_spectra(path, n, n_spec)
    built = power.shape[0]

    print("numpy oracle")
    print("  window    n=%d  sum=%.9f  w[0]=%.9g w[1]=%.9g w[n/2]=%.9f"
          % (n, w.sum(dtype=np.float64), w[0], w[1], w[n // 2]))
    print("  built     %d spectra, max_value = %.9g" % (built, power.max()))
    flat = power.ravel()
    idx = int(flat.size * 5.0 / 100.0)
    floor5 = np.partition(flat, idx)[idx]
    print("  floor     5th percentile = %.9g  (%.6f dB)" % (floor5, 10 * np.log10(floor5)))
    for s in range(min(3, built)):
        print("  spec[%d]   [0]=%.9g [1]=%.9g [n/2]=%.9g [n-1]=%.9g"
              % (s, power[s, 0], power[s, 1], power[s, n // 2], power[s, n - 1]))

    if cpp_dump:
        cpp = np.fromfile(cpp_dump, dtype=np.float64)
        if cpp.size != built * n:
            print("\nMISMATCH: C++ dump has %d values, expected %d" % (cpp.size, built * n))
            return 1
        cpp = cpp.reshape(built, n)
        diff = np.abs(cpp - power)
        denom = np.maximum(np.abs(power), 1e-300)
        rel = (diff / denom).max()
        print("\ncomparison vs C++")
        print("  max abs diff   %.6g" % diff.max())
        print("  max rel diff   %.6g" % rel)
        # float32 input and a double FFT: agreement should be near machine epsilon
        ok = rel < 1e-9
        print("  %s" % ("PASSED" if ok else "FAILED (relative error too large)"))
        return 0 if ok else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

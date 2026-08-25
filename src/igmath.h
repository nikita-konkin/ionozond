#ifndef IGMATH_H
#define IGMATH_H

#include <cstdio>
#include <vector>

/*
 * Numeric kernel of the ionogram builder.
 *
 * These routines are reconstructed from the original binary and kept in their
 * own translation unit so they can be tested without pulling in Qt or Qwt.
 * QRxIonogram / QIonogram call into them; the original had the same code
 * inline in those classes.
 *
 * Provenance of each routine is noted on the declaration.
 */

/*
 * getMedian(double*, int const&)  @0x471c80
 *
 * NOT a textbook median: the original ALWAYS averages the element at n/2
 * (after nth_element) with the maximum of the lower half, with no test for
 * odd n. For the power-of-two spectrum lengths used in practice n is even and
 * this coincides with the usual definition, but the behaviour is reproduced
 * exactly. The input array is not modified.
 */
double getMedian(const double *data, const int &count);

/*
 * QIonogram::calculateHanningWindow(unsigned int const&)  @0x479a90
 *
 * Periodic Hanning: w[k] = 0.5 * (1 - cos(2*pi*k / n)).
 * Note the division by n, not n-1 -- this is NOT numpy.hanning(), it matches
 * scipy.signal.get_window('hann', n, fftbins=True).
 *
 * The original computes the step 2*pi/n in double, narrows it to float, and
 * multiplies by (float)k; that narrowing is reproduced here so the window
 * matches bit for bit.
 */
std::vector<float> calculateHanningWindow(unsigned int n);

/* Rotate a spectrum by half its length, as np.fft.fftshift does. */
void fftShift(double *spec, int n);

/*
 * Core of QRxIonogram::readNewLfsSpecData(int const&, int const&)  @0x46e6f0
 *
 * Reads nSpec consecutive blocks of specPointCount complex64 samples from fp,
 * applies the window, runs one batched forward FFT, then per spectrum:
 *
 *     power[k] = re*re + im*im
 *     fftshift(power)
 *     power[k] /= getMedian(power)      <- noise-floor normalisation
 *     reverse(power)                    <- delay axis flip
 *
 * The reverse is not cosmetic. Dechirping against a local replica that runs
 * ahead of the received sweep makes the beat frequency go NEGATIVE as the
 * echo delay grows, so after fftshift the rows run from long delay to short.
 * Reversing puts them back in ascending-delay order, which is why row 0 of the
 * result is the most negative virtual height and row n-1 the most positive.
 * Confirmed on a real capture: the returned signal lands at +2655..+2809 km,
 * just beyond the 2550 km ray path (the opposite convention placed it at
 * -2684 km, which is unphysical).
 *
 * specPower must hold nSpec arrays of specPointCount doubles.
 * Returns the number of spectra actually produced, and reports the largest
 * normalised power seen through maxValue (the original's "max_value =" trace).
 */
int buildSpectra(FILE *fp,
                 int specPointCount,
                 const float *window,
                 int nSpec,
                 double **specPower,
                 double *maxValue);

/*
 * QIonogram::getPowerDynamicLimit(QIonogramDataArray const&, uint, uint)
 *                                                                @0x477a90
 *
 * Automatic per-spectrum noise threshold, in dB. This is a Rosin (triangle)
 * threshold:
 *
 *   1. bin the dB values into integer bins:  bin = (int)roundf(value),
 *      clamped at 0, giving `limit` = roundf(max) bins
 *   2. turn the histogram into a survival curve S(d) = count of points >= d,
 *      so S runs from N at d=0 down towards 0 at d=limit
 *   3. return the d in [1, limit) that maximises
 *
 *          N - (N/limit)*d - S(d)
 *
 *      i.e. the bin where the curve falls furthest below the straight chord
 *      from (0, N) to (limit, 0) -- the "corner" of the distribution.
 *
 * Ties go to the larger d, matching the original's >= comparison.
 */
float getPowerDynamicLimit(const float *spec, int count);

/*
 * QIonogram::applyHardPowerLimitToSpec(...)                       @0x478520
 *
 * Zeroes every point strictly below the limit; points at or above it are left
 * untouched. The original writes only the zeros, assuming the destination
 * already holds a copy of the source.
 */
void applyHardPowerLimitToSpec(float *dst, const float *src, int count, float limit);

/*
 * Display floor used by the original: the 5th percentile of all normalised
 * power values across the frame, i.e. nth_element at total * 5.0 / 100.0.
 * Takes a copy, so the caller's data is left alone.
 */
double percentileFloor(const double *const *specPower, int nSpec,
                       int specPointCount, double percent = 5.0);

#endif /* IGMATH_H */

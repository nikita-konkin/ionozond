#ifndef IGANALYTICS_H
#define IGANALYTICS_H

#include <QVector>

#include <qwt_samples.h>

/*
 * Derived products computed from a built ionogram: the usable-frequency band,
 * the per-spectrum signal/noise ratio, and the power-delay profile.
 *
 * All of these operate on the GATED dB array (see getPowerDynamicLimit in
 * igmath.h) -- points below the noise threshold are zero, and the algorithms
 * rely on that.
 */

/*
 * QIonogram::calculateUsageFrequencies() @0x478980
 *
 * Finds the lowest and highest usable frequency by looking for a run of three
 * consecutive spectra that each contain at least three consecutive non-zero
 * points:
 *
 *   LUF: scan spectra upward;   on the third qualifying spectrum, index -= 2
 *   MUF: scan spectra downward; on the third qualifying spectrum, index += 2
 *
 * Both indices are set to -1 when no such run exists, and the function does
 * nothing at all when there are fewer than 3 spectra or 3 points.
 */
struct UsageFrequencies {
    int lufIndex;
    int mufIndex;
    UsageFrequencies() : lufIndex(-1), mufIndex(-1) {}
    bool isValid() const { return lufIndex >= 0 && mufIndex >= 0; }
};

UsageFrequencies calculateUsageFrequencies(const float *const *data,
                                           int specCount, int specPointCount);

/*
 * QIonogram::calcAllSpecSnr() @0x479d30
 *
 * One SNR figure per spectrum, in dB:
 *
 *   noise = specMedianPower * 2*ln(2)          (1.3862943611198906)
 *   sum   = total of every point > 0
 *   snr   = 10*log10(sum / noise - 1)          only when sum/noise - 1 >= 1
 *
 * Spectra outside [lufIndex, mufIndex] get 0, as do spectra whose median is
 * zero or whose ratio falls below 1.
 */
QVector<float> calcAllSpecSnr(const float *const *data,
                              int specCount, int specPointCount,
                              const double *specMedianPower,
                              const UsageFrequencies &uf);

/*
 * QIonogram::getSnrVector(float const&) @0x479b80 / getBandSnr @0x478d70
 *
 * Averages the per-spectrum SNR over frequency bands of the requested width,
 * which is the "усреднение по частоте (кГц)" setting. The natural band width
 * is one spectrum:
 *
 *   QIonogram::getMinFreqBand() @0x478bf0
 *       = (freqMax - freqMin) * 1e6 / (specCount - 1)      [Hz per spectrum]
 *
 * so a requested band narrower than that has no effect. The returned vector
 * keeps one entry per spectrum, with every spectrum in a band sharing the
 * band average, so the frequency axis mapping is unchanged.
 */
QVector<float> averageSnrOverFreqBands(const QVector<float> &snr,
                                       double freqSpanMHz,
                                       double bandKHz);

/* Defaults from the call site in QIonogram::clear() @0x47d20e:
 *   OBJ_SIZE_HORIZONTAL_DEFAULT = 9   (spectra, the frequency direction)
 *   OBJ_SIZE_VERTICAL_DEFAULT   = 3   (points, the delay direction)
 *   LIMIT_DEFAULT               = 11.0f
 * A 9x3 window is 27 cells, so the threshold asks for ~41% occupancy. The
 * window being wide and thin matches the shape of an ionogram trace, which is
 * extended in frequency and narrow in delay. */
const unsigned int OBJ_SIZE_HORIZONTAL_DEFAULT = 9;
const unsigned int OBJ_SIZE_VERTICAL_DEFAULT   = 3;
const float        LIMIT_DEFAULT               = 11.0f;

/*
 * QIonogram::deleteSmallObjects(dst, src, w, h, level) @0x47cd10
 *
 * Speckle filter. In three steps:
 *
 *   1. counts[][] = for every cell, how many points of src that are > 0 lie
 *      within the w x h window centred on it
 *   2. mask = counts with everything below `level` zeroed
 *      (applyHardPowerLimits)
 *   3. dst = src where mask > 0, else 0            (fillMask)
 *
 * Note the original clamps the accumulation window to the interior band
 * [half, count-1-half] in both axes, so cells within half a window of an edge
 * never accumulate and are always removed. That edge erosion is reproduced.
 */
void deleteSmallObjects(float **dst, const float *const *src,
                        int specCount, int specPointCount,
                        unsigned int windowW = OBJ_SIZE_HORIZONTAL_DEFAULT,
                        unsigned int windowH = OBJ_SIZE_VERTICAL_DEFAULT,
                        float level = LIMIT_DEFAULT);

/*
 * QIonogram::fillMask(dst, src, mask) @0x478640
 *   dst[x][y] = mask[x][y] > 0 ? src[x][y] : 0
 */
void fillMask(float **dst, const float *const *src, const float *const *mask,
              int specCount, int specPointCount);

/*
 * QIonogram::medianEqualize(dst, src) @0x476aa0
 *   dst[x][y] = src[x][y] / specMedianPower(x)
 *
 * buildSpectra() already divides each spectrum by its own median, so on this
 * pipeline the stage is a no-op; it is provided for callers working with
 * un-normalised arrays.
 */
void medianEqualize(float **dst, const float *const *src,
                    int specCount, int specPointCount,
                    const double *specMedianPower);

/*
 * QIonogram::applyHardPowerLimits(dst, src, limit) @0x4785b0
 * The same limit applied to every spectrum.
 */
void applyHardPowerLimits(float **dst, const float *const *src,
                          int specCount, int specPointCount, float limit);

/*
 * QIonogram::deleteObjectsUnderNoiseLevel(dst, src) @0x478700
 *
 * A second, statistical gate applied after the Rosin one:
 *
 *   for each spectrum
 *       peak = max over its points
 *       if (peak > 0)
 *           applyHardPowerLimitToSpec(dst, src, spec,
 *                                     specMedianPower * 2*ln(2))
 *
 * It uses the same 1.3862943611198906 constant as calcAllSpecSnr.
 *
 * UNIT CAVEAT: the threshold is a multiple of the median POWER, which implies
 * QIonogramDataArray holds linear normalised power rather than dB. This
 * reconstruction keeps its arrays in dB (which is what the display needs), so
 * the caller passes the equivalent dB level. Because the spectra are already
 * median-normalised, that level is 10*log10(2*ln 2) = 1.416 dB -- milder than
 * the Rosin threshold, so in practice this stage removes little on top of it.
 */
void deleteObjectsUnderNoiseLevel(float **dst, const float *const *src,
                                  int specCount, int specPointCount,
                                  float noiseLevelDb);

/* 10*log10(2*ln 2): the noise gate above, expressed for a median-normalised
 * spectrum whose median is 1 by construction. */
double noiseLevelDbForNormalisedSpectra();

/*
 * Integrated power-delay profile, from
 * QIonogram::calcShortAndIntegratedPowerDelayProfiles() @0x47ad60.
 *
 * For each delay row, the power is summed across spectra -- but only across
 * spectra inside the usable band, between getLufIndex() and getMufIndex().
 * Each sample covers a delay interval CENTRED on its row: the original takes
 * getValueY(i+1) - getValueY(i) and halves it (the 0.5f at @0x494478).
 *
 * Decoded: the luf..muf restriction, the summation, and the centred bins.
 * Still not decoded: the companion "short" profile, which uses a separate
 * QShortPowerDelayProfileSample type and is not needed by the ПЗМ panel.
 */
QVector<QwtIntervalSample> calcIntegratedPowerDelayProfile(
    const float *const *data, int specCount, int specPointCount,
    double delayMinMs, double delayMaxMs,
    const UsageFrequencies &uf);

#endif /* IGANALYTICS_H */

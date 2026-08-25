#include "iganalytics.h"

#include "igmath.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

/* A spectrum "has signal" when it holds three consecutive non-zero points. */
bool hasRun(const float *spec, int n, int runLength = 3)
{
    int run = 0;
    for (int k = 0; k < n; ++k) {
        if (spec[k] > 0.0f) {
            if (++run >= runLength)
                return true;
        } else {
            run = 0;
        }
    }
    return false;
}

} // namespace

UsageFrequencies calculateUsageFrequencies(const float *const *data,
                                           int specCount, int specPointCount)
{
    UsageFrequencies uf;
    if (!data || specCount <= 2 || specPointCount <= 2)
        return uf;

    /* Forward pass: the third consecutive qualifying spectrum fixes the LUF,
     * backed up by two to the first of the run. */
    int run = 0;
    for (int s = 0; s < specCount; ++s) {
        if (hasRun(data[s], specPointCount)) {
            if (++run == 3) {
                uf.lufIndex = s - 2;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (uf.lufIndex < 0)
        return uf;

    /* Backward pass, and forward by two to the last of the run. */
    run = 0;
    for (int s = specCount - 1; s >= 0; --s) {
        if (hasRun(data[s], specPointCount)) {
            if (++run == 3) {
                uf.mufIndex = s + 2;
                break;
            }
        } else {
            run = 0;
        }
    }
    return uf;
}

QVector<float> calcAllSpecSnr(const float *const *data,
                              int specCount, int specPointCount,
                              const double *specMedianPower,
                              const UsageFrequencies &uf)
{
    QVector<float> snr;
    snr.reserve(specCount);
    if (!data)
        return snr;

    /* median -> mean power for an exponential distribution */
    const double NOISE_FACTOR = 1.3862943611198906;   /* 2*ln(2) */

    for (int s = 0; s < specCount; ++s) {
        float value = 0.0f;

        const bool inBand = uf.isValid() &&
                            s >= uf.lufIndex && s <= uf.mufIndex;
        if (inBand) {
            const double median = specMedianPower ? specMedianPower[s] : 0.0;
            if (median != 0.0) {
                const float noise = (float)(median * NOISE_FACTOR);

                float sum = 0.0f;
                for (int k = 0; k < specPointCount; ++k)
                    if (data[s][k] > 0.0f)
                        sum += data[s][k];

                const float ratio = sum / noise - 1.0f;
                if (ratio >= 1.0f)
                    value = (float)(10.0 * std::log10((double)ratio));
            }
        }
        snr.append(value);
    }
    return snr;
}

QVector<QwtIntervalSample> calcIntegratedPowerDelayProfile(
    const float *const *data, int specCount, int specPointCount,
    double delayMinMs, double delayMaxMs,
    const UsageFrequencies &uf)
{
    QVector<QwtIntervalSample> pdp;
    if (!data || specCount <= 0 || specPointCount <= 0)
        return pdp;

    /* Only spectra inside the usable band contribute. */
    int first = 0;
    int last = specCount - 1;
    if (uf.isValid()) {
        first = qBound(0, uf.lufIndex, specCount - 1);
        last  = qBound(0, uf.mufIndex, specCount - 1);
        if (last < first)
            return pdp;
    }

    const double step = (delayMaxMs - delayMinMs) / (double)(specPointCount - 1);
    const double half = step * 0.5;      /* @0x494478 */
    pdp.reserve(specPointCount);

    for (int k = 0; k < specPointCount; ++k) {
        double sum = 0.0;
        for (int s = first; s <= last; ++s)
            if (data[s][k] > 0.0f)
                sum += data[s][k];

        const double y = delayMinMs + step * k;
        pdp.append(QwtIntervalSample(sum, QwtInterval(y - half, y + half)));
    }
    return pdp;
}

QVector<float> averageSnrOverFreqBands(const QVector<float> &snr,
                                       double freqSpanMHz,
                                       double bandKHz)
{
    const int n = snr.size();
    if (n <= 1 || bandKHz <= 0.0 || freqSpanMHz <= 0.0)
        return snr;

    /* getMinFreqBand(): Hz covered by one spectrum. */
    const double minBandHz = freqSpanMHz * 1e6 / (double)(n - 1);
    const double wantedHz = bandKHz * 1e3;
    if (wantedHz <= minBandHz)
        return snr;                       /* narrower than one spectrum */

    const int perBand = (int)(wantedHz / minBandHz);
    if (perBand <= 1)
        return snr;

    QVector<float> out(n, 0.0f);
    for (int start = 0; start < n; start += perBand) {
        const int stop = qMin(start + perBand, n);

        double sum = 0.0;
        int used = 0;
        for (int i = start; i < stop; ++i) {
            if (snr.at(i) > 0.0f) {       /* zeros are out-of-band markers */
                sum += snr.at(i);
                ++used;
            }
        }
        const float mean = used ? (float)(sum / used) : 0.0f;
        for (int i = start; i < stop; ++i)
            out[i] = mean;
    }
    return out;
}

double noiseLevelDbForNormalisedSpectra()
{
    /* median is 1 after normalisation, so the level is just the factor. */
    return 10.0 * std::log10(1.3862943611198906);
}

void deleteObjectsUnderNoiseLevel(float **dst, const float *const *src,
                                  int specCount, int specPointCount,
                                  float noiseLevelDb)
{
    if (!dst || !src)
        return;

    for (int s = 0; s < specCount; ++s) {
        /* peak of the spectrum; spectra that are entirely empty are skipped */
        float peak = src[s][0];
        for (int k = 1; k < specPointCount; ++k)
            if (src[s][k] > peak)
                peak = src[s][k];

        if (peak > 0.0f)
            applyHardPowerLimitToSpec(dst[s], src[s], specPointCount, noiseLevelDb);
    }
}

void fillMask(float **dst, const float *const *src, const float *const *mask,
              int specCount, int specPointCount)
{
    if (!dst || !src || !mask)
        return;
    for (int x = 0; x < specCount; ++x)
        for (int y = 0; y < specPointCount; ++y)
            dst[x][y] = (mask[x][y] > 0.0f) ? src[x][y] : 0.0f;
}

void medianEqualize(float **dst, const float *const *src,
                    int specCount, int specPointCount,
                    const double *specMedianPower)
{
    if (!dst || !src)
        return;
    for (int x = 0; x < specCount; ++x) {
        const double median = specMedianPower ? specMedianPower[x] : 1.0;
        if (median == 0.0)
            continue;
        for (int y = 0; y < specPointCount; ++y)
            dst[x][y] = (float)(src[x][y] / median);
    }
}

void applyHardPowerLimits(float **dst, const float *const *src,
                          int specCount, int specPointCount, float limit)
{
    if (!dst || !src)
        return;
    for (int x = 0; x < specCount; ++x)
        applyHardPowerLimitToSpec(dst[x], src[x], specPointCount, limit);
}

void deleteSmallObjects(float **dst, const float *const *src,
                        int specCount, int specPointCount,
                        unsigned int windowW, unsigned int windowH,
                        float level)
{
    if (!dst || !src || specCount <= 0 || specPointCount <= 0)
        return;

    const int halfW = (int)(windowW / 2);
    const int halfH = (int)(windowH / 2);

    /* 1. neighbour counts */
    std::vector<std::vector<float> > counts(
        specCount, std::vector<float>(specPointCount, 0.0f));

    /* The window is clamped to the interior band in both axes, exactly as the
     * original does -- cells nearer an edge than half a window never gain a
     * count and are therefore always removed. */
    const int xLoBound = halfW;
    const int xHiBound = specCount - halfW - 1;
    const int yLoBound = halfH;
    const int yHiBound = specPointCount - halfH - 1;

    for (int x = 0; x < specCount; ++x) {
        for (int y = 0; y < specPointCount; ++y) {
            if (!(src[x][y] > 0.0f))
                continue;

            const int xLo = std::max(x - halfW, xLoBound);
            const int xHi = std::min(x + halfW, xHiBound);
            const int yLo = std::max(y - halfH, yLoBound);
            const int yHi = std::min(y + halfH, yHiBound);
            if (xLo > xHi || yLo > yHi)
                continue;

            for (int xx = xLo; xx <= xHi; ++xx)
                for (int yy = yLo; yy <= yHi; ++yy)
                    counts[xx][yy] += 1.0f;
        }
    }

    /* 2. mask = counts hard-limited at `level` */
    std::vector<const float *> countRows(specCount);
    for (int x = 0; x < specCount; ++x)
        countRows[x] = counts[x].data();

    std::vector<std::vector<float> > mask(
        specCount, std::vector<float>(specPointCount, 0.0f));
    std::vector<float *> maskRows(specCount);
    std::vector<const float *> maskConstRows(specCount);
    for (int x = 0; x < specCount; ++x) {
        mask[x] = counts[x];
        maskRows[x] = mask[x].data();
        maskConstRows[x] = mask[x].data();
    }
    applyHardPowerLimits(maskRows.data(), countRows.data(),
                         specCount, specPointCount, level);

    /* 3. keep src where the mask survived */
    fillMask(dst, src, maskConstRows.data(), specCount, specPointCount);
}

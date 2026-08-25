#include "igmath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <fftw3.h>

double getMedian(const double *data, const int &count)
{
    /* Original: new double[n], memcpy, nth_element, then max of lower half. */
    std::vector<double> tmp(data, data + count);

    const int mid = count / 2;
    std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
    const double middle = tmp[mid];

    /* max over [0, mid) -- and note there is no odd/even branch. */
    double lower = tmp[0];
    for (int i = 1; i < mid; ++i)
        lower = std::max(tmp[i], lower);

    return (lower + middle) * 0.5;
}

std::vector<float> calculateHanningWindow(unsigned int n)
{
    std::vector<float> window;
    window.reserve(n);

    /* The step is narrowed to float before use, exactly as the original does. */
    const float step = static_cast<float>(6.283185307179586 / static_cast<double>(n));

    for (unsigned int k = 0; k < n; ++k) {
        const double arg = static_cast<double>(static_cast<float>(k) * step);
        window.push_back(static_cast<float>(0.5 * (1.0 - std::cos(arg))));
    }
    return window;
}

void fftShift(double *spec, int n)
{
    /* Original does this with a stack copy and three memcpys; half = (n+1)/2. */
    const int half = (n + 1) / 2;
    std::vector<double> tmp(spec, spec + n);
    std::memcpy(spec, tmp.data() + half, (size_t)(n - half) * sizeof(double));
    std::memcpy(spec + (n - half), tmp.data(), (size_t)half * sizeof(double));
}

int buildSpectra(FILE *fp,
                 int specPointCount,
                 const float *window,
                 int nSpec,
                 double **specPower,
                 double *maxValue)
{
    if (nSpec <= 0 || specPointCount <= 0)
        return 0;

    fftw_complex *buf =
        (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * (size_t)nSpec * specPointCount);
    if (!buf)
        return 0;

    /* Read each block and apply the window while converting to double. */
    std::vector<float> raw((size_t)specPointCount * 2);
    int read = 0;
    for (; read < nSpec; ++read) {
        std::fill(raw.begin(), raw.end(), 0.0f);
        size_t got = std::fread(raw.data(), 8, (size_t)specPointCount, fp);
        if (got == 0)
            break;

        fftw_complex *dst = buf + (size_t)read * specPointCount;
        for (int k = 0; k < specPointCount; ++k) {
            dst[k][0] = (double)(window[k] * raw[2 * k]);
            dst[k][1] = (double)(window[k] * raw[2 * k + 1]);
        }
    }
    if (read == 0) {
        fftw_free(buf);
        return 0;
    }

    int n = specPointCount;
    fftw_plan plan = fftw_plan_many_dft(1, &n, read,
                                        buf, &n, 1, n,
                                        buf, &n, 1, n,
                                        FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(plan);
    fftw_destroy_plan(plan);

    double maxSeen = 0.0;
    for (int s = 0; s < read; ++s) {
        const fftw_complex *src = buf + (size_t)s * specPointCount;
        double *power = specPower[s];

        for (int k = 0; k < specPointCount; ++k)
            power[k] = src[k][0] * src[k][0] + src[k][1] * src[k][1];

        fftShift(power, specPointCount);

        const double median = getMedian(power, specPointCount);
        for (int k = 0; k < specPointCount; ++k) {
            power[k] /= median;
            if (power[k] > maxSeen)
                maxSeen = power[k];
        }

        std::reverse(power, power + specPointCount);
    }

    fftw_free(buf);
    if (maxValue)
        *maxValue = maxSeen;
    return read;
}

double percentileFloor(const double *const *specPower, int nSpec,
                       int specPointCount, double percent)
{
    const size_t total = (size_t)nSpec * specPointCount;
    if (total == 0)
        return 0.0;

    std::vector<double> flat;
    flat.reserve(total);
    for (int s = 0; s < nSpec; ++s)
        flat.insert(flat.end(), specPower[s], specPower[s] + specPointCount);

    const size_t idx = (size_t)((double)total * percent / 100.0);
    std::nth_element(flat.begin(), flat.begin() + idx, flat.end());
    return flat[idx];
}

float getPowerDynamicLimit(const float *spec, int count)
{
    if (!spec || count <= 0)
        return 0.0f;

    float maxValue = spec[0];
    for (int i = 1; i < count; ++i)
        if (spec[i] > maxValue)
            maxValue = spec[i];

    const int limit = (int)std::floor(maxValue + 0.5f);   /* roundf */
    if (limit < 2)
        return 0.0f;

    /* 1. integer-dB histogram, negatives folded into bin 0 */
    std::vector<int> hist(limit + 1, 0);
    for (int i = 0; i < count; ++i) {
        int bin = (int)std::floor(spec[i] + 0.5f);
        if (bin < 0)     bin = 0;
        if (bin > limit) bin = limit;
        ++hist[bin];
    }

    /* 2. survival curve: hist[c] becomes the number of points >= c */
    for (int c = limit - 1; c >= 0; --c)
        hist[c] += hist[c + 1];

    /* 3. bin furthest below the chord from (0, N) to (limit, 0) */
    const float n = (float)count;
    const float slope = -n / (float)limit;

    float best = 0.0f;
    float bestD = 0.0f;
    for (int d = 1; d < limit; ++d) {
        const float deviation = slope * (float)d + n - (float)hist[d];
        if (deviation >= best) {      /* >= : ties go to the larger d */
            best = deviation;
            bestD = (float)d;
        }
    }
    return bestD;
}

void applyHardPowerLimitToSpec(float *dst, const float *src, int count, float limit)
{
    if (!dst || !src)
        return;
    for (int i = 0; i < count; ++i) {
        if (limit > src[i])
            dst[i] = 0.0f;
    }
}

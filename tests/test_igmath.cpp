/*
 * Verification 0: differential test of the numeric kernel against the ORIGINAL
 * binary.
 *
 * The expected values below were produced by calling getMedian() inside
 * dsChirp/bin/dsChirp itself under gdb -- see tools/oracle_getmedian.gdb.
 * They are not hand-computed, they are what the shipped binary returns.
 *
 * Note the odd-n cases: a textbook median of {5,1,9,3,7,2,8} is 5, but the
 * original returns 4 because it unconditionally averages the n/2 element with
 * the max of the lower half. The reconstruction must reproduce that.
 */
#include "../src/igmath.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;

static void expect(const char *what, double got, double want, double tol = 1e-12)
{
    const bool ok = std::fabs(got - want) < tol;
    std::printf("  %-24s got %-10.6g want %-10.6g  %s\n",
                what, got, want, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

static void test_getmedian_vs_original()
{
    std::printf("getMedian vs original binary (gdb oracle):\n");
    const double data[8] = { 5.0, 1.0, 9.0, 3.0, 7.0, 2.0, 8.0, 4.0 };

    int n = 8; expect("even n=8", getMedian(data, n), 4.5);
    n = 7;     expect("odd  n=7", getMedian(data, n), 4.0);
    n = 5;     expect("odd  n=5", getMedian(data, n), 4.0);
    n = 2;     expect("n=2",      getMedian(data, n), 3.0);
}

static void test_getmedian_leaves_input_alone()
{
    std::printf("getMedian does not modify its input:\n");
    const double src[5] = { 5.0, 1.0, 9.0, 3.0, 7.0 };
    double data[5];
    for (int i = 0; i < 5; ++i) data[i] = src[i];
    int n = 5;
    getMedian(data, n);
    bool same = true;
    for (int i = 0; i < 5; ++i) if (data[i] != src[i]) same = false;
    std::printf("  %-24s %s\n", "input preserved", same ? "ok" : "FAIL");
    if (!same) ++failures;
}

static void test_hanning()
{
    std::printf("periodic Hanning window:\n");
    const unsigned n = 16;
    std::vector<float> w = calculateHanningWindow(n);

    std::printf("  %-24s %zu\n", "size", w.size());
    if (w.size() != n) { ++failures; return; }

    expect("w[0]", w[0], 0.0);
    /* periodic: w[n/2] is exactly 1, and w[k] == w[n-k] */
    expect("w[n/2]", w[n / 2], 1.0);
    bool symmetric = true;
    for (unsigned k = 1; k < n / 2; ++k)
        if (std::fabs(w[k] - w[n - k]) > 1e-6f) symmetric = false;
    std::printf("  %-24s %s\n", "symmetry w[k]==w[n-k]", symmetric ? "ok" : "FAIL");
    if (!symmetric) ++failures;

    /* periodic Hanning sums to n/2; the symmetric variant would not */
    double sum = 0.0;
    for (unsigned k = 0; k < n; ++k) sum += w[k];
    expect("sum == n/2", sum, n / 2.0, 1e-5);  /* float32 window */
}

static void test_fftshift()
{
    std::printf("fftShift:\n");
    double even[4] = { 1, 2, 3, 4 };
    fftShift(even, 4);
    bool ok = (even[0] == 3 && even[1] == 4 && even[2] == 1 && even[3] == 2);
    std::printf("  %-24s %s\n", "n=4 -> {3,4,1,2}", ok ? "ok" : "FAIL");
    if (!ok) ++failures;

    double odd[5] = { 1, 2, 3, 4, 5 };
    fftShift(odd, 5);
    /* half = (5+1)/2 = 3, so {4,5,1,2,3} -- matches np.fft.fftshift */
    ok = (odd[0] == 4 && odd[1] == 5 && odd[2] == 1 && odd[3] == 2 && odd[4] == 3);
    std::printf("  %-24s %s\n", "n=5 -> {4,5,1,2,3}", ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

/*
 * getPowerDynamicLimit: the Rosin/triangle threshold. Constructed cases where
 * the answer is determined by the algorithm's definition rather than by any
 * particular data set.
 */
static void test_power_dynamic_limit()
{
    std::printf("getPowerDynamicLimit (Rosin threshold):\n");

    /* Degenerate: nothing above 2 dB means no usable histogram. */
    {
        std::vector<float> flat(100, 1.0f);
        expect("flat input -> 0", getPowerDynamicLimit(flat.data(), (int)flat.size()), 0.0);
    }

    /* A big noise cluster near 0 dB plus a few strong points: the corner of
     * the survival curve should land above the noise and below the signal. */
    {
        std::vector<float> v;
        for (int i = 0; i < 1000; ++i) v.push_back(float(i % 3));   /* 0..2 dB */
        for (int i = 0; i < 10; ++i)   v.push_back(30.0f);          /* signal  */
        const float limit = getPowerDynamicLimit(v.data(), (int)v.size());
        const bool ok = (limit > 2.0f && limit < 30.0f);
        std::printf("  %-24s got %.1f  (expect 2 < x < 30)  %s\n",
                    "noise+signal", limit, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }

    /* The gate keeps values at or above the limit and zeroes the rest. */
    {
        const float src[6] = { 0.0f, 5.0f, 9.9f, 10.0f, 10.1f, 50.0f };
        float dst[6];
        for (int i = 0; i < 6; ++i) dst[i] = src[i];
        applyHardPowerLimitToSpec(dst, src, 6, 10.0f);
        const bool ok = dst[0] == 0.0f && dst[1] == 0.0f && dst[2] == 0.0f &&
                        dst[3] == 10.0f && dst[4] == 10.1f && dst[5] == 50.0f;
        std::printf("  %-24s %s\n", "gate zeroes below limit", ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
}

int main()
{
    test_getmedian_vs_original();
    test_getmedian_leaves_input_alone();
    test_hanning();
    test_fftshift();
    test_power_dynamic_limit();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

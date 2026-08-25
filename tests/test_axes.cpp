/*
 * Verification 5: ionogram axis geometry.
 *
 * The original, running with cyprus1 -> yoshkar-ola, displayed
 *   f = 7.5 .. 32.5 MHz   and   t = 8 .. 13.5 ms
 * (tests/out/oracle_main.png). Those ranges must fall out of the recovered
 * formulas and the real capture's header alone.
 */
#include "../src/common.h"
#include "../src/lfs_header.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

static int failures = 0;

static void near(const char *what, double got, double want, double tol)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("  %-30s %10.4f  (expect %.4f +/- %.2f)  %s\n",
                what, got, want, tol, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const char *path = argc > 1 ? argv[1] : "/data/cyprus1_20191023_071510.lfs";
    FILE *fp = std::fopen(path, "rb");
    if (!fp) { std::printf("cannot open %s\n", path); return 1; }
    lfs_header h;
    if (!lfsheader_read(fp, h)) { std::printf("bad lfs\n"); std::fclose(fp); return 1; }
    std::fclose(fp);

    std::printf("axes for %s -> %s\n", h.tx_name, h.rx_name);

    /* Frequency sweep: starts half a band below the centre and climbs at the
     * chirp rate for the whole session. */
    const double freqStartHz = (double)h.cf - (double)h.sample_rate / 2.0;
    const double freqStopHz  = freqStartHz + (double)h.dur * (double)h.rate;
    near("freq start, MHz", freqStartHz / HZ_IN_MHZ, 7.5, 0.01);
    near("freq stop,  MHz", freqStopHz / HZ_IN_MHZ, 32.5, 0.01);

    /* Full unambiguous delay range: +/- (if_rate/2)/rate, as a light-path. */
    const double ifRate = (double)h.sample_rate / (double)h.dec;
    const double vrangeKm = LIGHT_SPEED_KM_S * (ifRate / 2.0) / (double)h.rate;
    near("if_rate, Hz", ifRate, 40000.0, 0.5);
    near("virtual height max, km", vrangeKm, (double)VIRT_HEIGHT_MAX, 1.0);

    /* Display window around the great-circle path. */
    const double dKm    = earthDistanceKm(h.tx_latitude, h.tx_longitude,
                                          h.rx_latitude, h.rx_longitude);
    const double rayKm  = rayDistanceKm(dKm);
    const double rminKm = rayKm - VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;
    const double rmaxKm = rminKm + VIRT_HEIGHT_WINDOW_KM_DEFAULT
                                 + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;

    std::printf("  %-30s %10.2f km\n", "great-circle tx->rx", dKm);
    std::printf("  %-30s %10.2f km\n", "ray distance", rayKm);

    const double tminMs = timeMsFromHeightKm(rminKm);
    const double tmaxMs = timeMsFromHeightKm(rmaxKm);

    /*
     * The original's y axis carried ticks 8..13 with the plot extending a
     * little past each end, i.e. roughly 7.9 .. 13.5 ms. Assert that band
     * rather than a hand-computed distance.
     */
    near("window t min, ms", tminMs, 7.90, 0.10);
    near("window t max, ms", tmaxMs, 13.50, 0.10);

    /* The window height is fixed by the two constants, independent of path. */
    near("window span, ms", tmaxMs - tminMs,
         timeMsFromHeightKm(VIRT_HEIGHT_WINDOW_KM_DEFAULT
                            + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT), 1e-9);

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#include "../src/common.h"

#include <qwt_scale_div.h>
#include <qwt_scale_engine.h>

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check(const char *what, bool ok)
{
    std::printf("  %-42s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

static void checkScale(double minTimeMs, double maxTimeMs, double expectedStepKm)
{
    std::printf("vertical scale for %g .. %g ms\n", minTimeMs, maxTimeMs);

    const double stepKm = ionogramHeightStepKm(minTimeMs, maxTimeMs);
    check("round kilometre step", stepKm == expectedStepKm);
    if (stepKm <= 0.0)
        return;

    const QwtLinearScaleEngine engine;
    const QwtScaleDiv scale = engine.divideScale(minTimeMs, maxTimeMs, 8, 0,
                                                 timeMsFromHeightKm(stepKm));
    const QList<double> &ticks = scale.ticks(QwtScaleDiv::MajorTick);
    check("4 to 10 major ticks in the visible range", ticks.size() >= 4 && ticks.size() <= 10);

    bool aligned = true;
    for (double tickMs : ticks) {
        const double tickKm = heightKmFromTimeMs(tickMs);
        const double multiple = std::round(tickKm / stepKm);
        if (std::fabs(tickKm - multiple * stepKm) >= 1e-9)
            aligned = false;
    }
    check("major ticks align to the kilometre step", aligned);
}

int main()
{
    checkScale(8.0, 13.5, 250.0);
    checkScale(0.0, 30.0, 2000.0);

    check("one millisecond is 300 kilometres", std::fabs(heightKmFromTimeMs(1.0) - 300.0) < 1e-9);
    check("milliseconds round-trip",
          std::fabs(timeMsFromHeightKm(heightKmFromTimeMs(13.5)) - 13.5) < 1e-9);
    check("kilometres round-trip",
          std::fabs(heightKmFromTimeMs(timeMsFromHeightKm(250.0)) - 250.0) < 1e-9);

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

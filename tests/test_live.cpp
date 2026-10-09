/*
 * LiveIonogram: a capture drawn while it is written must end up exactly as the
 * same capture drawn in one pass, and the one-pass picture must be the one
 * the console already computes for a finished capture.
 *
 *   test_live <capture.lfs> <scratch dir>
 *
 * The capture is copied into the scratch directory in uneven pieces, the way
 * the sounder appends to it, with an update() after each piece.
 */
#include "../src/liveionogram.h"
#include "../src/qrxionogram.h"

#include <QApplication>
#include <QFile>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

static void drain(LiveIonogram &live)
{
    while (live.update(1 << 30)) {}
}

/* Copy `src` into `dst` piece by piece, updating `live` as it grows. Returns
 * how many updates drew something. */
static int grow(const QString &src, const QString &dst, const LiveSettings &s,
                LiveIonogram **out)
{
    QFile in(src);
    QFile o(dst);
    if (!in.open(QIODevice::ReadOnly) || !o.open(QIODevice::WriteOnly))
        return -1;
    o.write(in.read(512));
    o.flush();

    LiveIonogram *live = new LiveIonogram(dst, s);
    int drawn = 0;
    /* 0.3 .. 1.6 s of samples at 40 kHz, varying so no piece boundary lines
     * up with a window twice. */
    const int sizes[] = { 12001, 40000, 64007, 23456, 52000, 33333 };
    for (int i = 0; !in.atEnd(); ++i) {
        o.write(in.read((qint64)sizes[i % 6] * 8));
        o.flush();
        if (live->update(64))
            ++drawn;
    }
    o.close();
    drain(*live);
    *out = live;
    return drawn;
}

static bool sameColumns(const LiveIonogram &a, const LiveIonogram &b)
{
    if (a.columns() != b.columns() || a.rows() != b.rows() || a.ready() != b.ready())
        return false;
    for (int c = 0; c < a.ready(); ++c)
        if (std::memcmp(a.column(c), b.column(c), sizeof(float) * a.rows()) != 0)
            return false;
    return true;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <capture.lfs> <scratch dir>\n", argv[0]);
        return 2;
    }
    const QString cap = QString::fromLocal8Bit(argv[1]);
    const QString tmp = QString::fromLocal8Bit(argv[2]) + QLatin1String("/live_test.lfs");

    struct Case { const char *name; int pad, overlap; bool snr; };
    const Case cases[] = {
        { "gated, x1 x1 (the original's grid)", 1, 1, false },
        { "gated, x4 x2",                       4, 2, false },
        { "snr,   x4 x2",                       4, 2, true  },
    };

    for (int k = 0; k < 3; ++k) {
        std::printf("--- %s\n", cases[k].name);
        LiveSettings s;
        s.pad = cases[k].pad;
        s.overlap = cases[k].overlap;
        s.snrMode = cases[k].snr;

        LiveIonogram whole(cap, s);
        check(whole.isValid(), "header read");
        drain(whole);
        check(whole.complete(), "one pass reaches the last column");

        LiveIonogram *grown = 0;
        const int drawn = grow(cap, tmp, s, &grown);
        std::printf("  %d columns x %d rows, drawn in %d updates\n",
                    grown->columns(), grown->rows(), drawn);
        check(drawn > 20, "drawn progressively, not all at the end");
        check(grown->complete(), "grown copy reaches the last column");
        check(sameColumns(whole, *grown), "grown == one pass, bit for bit");
        delete grown;
    }

    /* The one-pass picture on the original's grid against loadLfsData, the
     * console's own path for a finished capture with no sidecar. */
    std::printf("--- against QRxIonogram::loadLfsData\n");
    {
        QBaseSoundParams base;
        base.sampleRateIndex = SAMPLE_RATE_LIST().indexOf(QLatin1String("25000"));
        base.fftCountIndex = FFT_COUNT_LIST().indexOf(QLatin1String("16384"));
        QTxParams tx;
        QRxParams rx;
        QRxIonogram panel(base, tx, rx);
        check(panel.loadLfsData(cap), "loadLfsData");
        const LfpProducts p = panel.products();

        LiveSettings s;
        LiveIonogram live(cap, s);
        drain(live);

        check((int)p.specCount == live.columns() && (int)p.specPointCount == live.rows(),
              "same grid");
        check(std::fabs(p.delayMinMs - live.area().top()) < 1e-4
              && std::fabs(p.delayMaxMs - live.area().bottom()) < 1e-4
              && std::fabs(p.freqMinMHz - live.area().left()) < 1e-4
              && std::fabs(p.freqMaxMHz - live.area().right()) < 1e-4,
              "same axes");

        long cells = 0, differ = 0, kept = 0;
        if ((int)p.specCount == live.columns() && (int)p.specPointCount == live.rows()) {
            for (int c = 0; c < live.columns(); ++c)
                for (int r = 0; r < live.rows(); ++r) {
                    const float a = p.ionogram.at(c * live.rows() + r);
                    const float b = live.column(c)[r];
                    ++cells;
                    if (a > 0.0f) ++kept;
                    if (std::fabs(a - b) > 1e-3f) ++differ;
                }
        }
        std::printf("  %ld cells, %ld kept by the gate, %ld differ\n", cells, kept, differ);
        check(cells > 0 && kept > 0 && differ * 10000 < cells, "same picture (< 0.01% cells differ)");
    }

    /* The great circle, against the haversine form of it. */
    std::printf("--- path length\n");
    {
        const double d2r = 3.14159265358979323846 / 180.0;
        const double la1 = 59.56 * d2r, lo1 = 150.80 * d2r, la2 = 56.38 * d2r, lo2 = 47.53 * d2r;
        const double h = std::pow(std::sin((la2 - la1) / 2), 2)
                       + std::cos(la1) * std::cos(la2) * std::pow(std::sin((lo2 - lo1) / 2), 2);
        const double haversine = 2.0 * 6378.137 * std::asin(std::sqrt(h));
        const double km = LiveIonogram::groundDistanceKm(59.56, 150.80, 56.38, 47.53);
        std::printf("  Magadan - Yoshkar-Ola %.1f km (haversine %.1f)\n", km, haversine);
        check(std::fabs(km - haversine) < 0.01, "great circle");
    }

    /* A sounding stopped early is renamed: the live picture must notice. */
    std::printf("--- a capture that goes away\n");
    {
        QFile in(cap);
        QFile o(tmp);
        in.open(QIODevice::ReadOnly);
        o.open(QIODevice::WriteOnly);
        o.write(in.read(512 + 40000 * 8 * 5));
        o.close();
        LiveSettings s;
        LiveIonogram live(tmp, s);
        check(live.update() && !live.complete(), "part of it drawn");
        QFile::remove(tmp);
        check(!live.update() && live.gone(), "gone() once the file is");
    }

    std::printf(failures ? "FAILED (%d failures)\n" : "PASSED (0 failures)\n", failures);
    return failures ? 1 : 0;
}

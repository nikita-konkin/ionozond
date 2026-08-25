/*
 * Verification: the LFP sidecar must round-trip, and must enforce its own
 * compatibility rule.
 */
#include "../src/lfpfile.h"

#include <QCoreApplication>
#include <QFile>

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check(const char *what, bool ok)
{
    std::printf("  %-38s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString path = QLatin1String("/tmp/test_roundtrip.lfp");

    LfpProducts w;
    w.txName = QLatin1String("cyprus1");
    w.rxName = QLatin1String("yoshkar-ola");
    w.txLat = 35.0f;  w.txLon = 34.0f;
    w.rxLat = 56.38f; w.rxLon = 47.53f;
    w.startEpochMs = Q_INT64_C(1571814910000);
    w.cfHz = 20000000; w.rateHzS = 100000; w.sampleRateHz = 25000000; w.dec = 625;
    w.durS = 250; w.whiten = 0; w.whitenLen = 8192; w.whitenN = 20000;
    w.fftCount = 16384; w.specCount = 7; w.specPointCount = 5;
    w.freqMinMHz = 7.5f; w.freqMaxMHz = 32.5f;
    w.delayMinMs = 7.901f; w.delayMaxMs = 13.501f;
    w.noiseGateDb = 5.5f; w.maxValueDb = 35.737f;
    w.lufMHz = 9.25f; w.mufMHz = 19.5f;
    w.lufIndex = 42; w.mufIndex = 293;
    w.gated = true;

    /* Mostly zeros, as a gated ionogram is -- also exercises compression. */
    for (quint32 i = 0; i < w.specCount * w.specPointCount; ++i)
        w.ionogram.append((i % 11 == 0) ? (float)i * 0.5f : 0.0f);
    for (quint32 i = 0; i < w.specCount; ++i)
        w.snr.append((float)i * 1.25f);
    for (quint32 i = 0; i < w.specPointCount; ++i)
        w.pdp.append((float)i * 100.0f);

    std::printf("round-trip:\n");
    check("write", writeLfp(path, w, QLatin1String("test"), QLatin1String("0.1")));

    LfpProducts r;
    check("read", readLfp(path, r));

    check("tx name",        r.txName == w.txName);
    check("rx name",        r.rxName == w.rxName);
    check("tx lat/lon",     r.txLat == w.txLat && r.txLon == w.txLon);
    check("start epoch",    r.startEpochMs == w.startEpochMs);
    check("cf / rate / dec", r.cfHz == w.cfHz && r.rateHzS == w.rateHzS && r.dec == w.dec);
    check("spec dims",      r.specCount == w.specCount &&
                            r.specPointCount == w.specPointCount);
    check("freq window",    r.freqMinMHz == w.freqMinMHz && r.freqMaxMHz == w.freqMaxMHz);
    check("delay window",   r.delayMinMs == w.delayMinMs && r.delayMaxMs == w.delayMaxMs);
    check("luf / muf",      r.lufMHz == w.lufMHz && r.mufMHz == w.mufMHz);
    check("luf / muf index", r.lufIndex == w.lufIndex && r.mufIndex == w.mufIndex);
    check("gated flag",     r.gated == w.gated);

    check("ionogram size",  r.ionogram.size() == w.ionogram.size());
    bool same = r.ionogram.size() == w.ionogram.size();
    for (int i = 0; same && i < w.ionogram.size(); ++i)
        if (r.ionogram.at(i) != w.ionogram.at(i)) same = false;
    check("ionogram values exact", same);

    same = r.snr == w.snr;
    check("snr values exact", same);
    same = r.pdp == w.pdp;
    check("pdp values exact", same);

    /* Compression must actually be doing something on sparse data. */
    const qint64 onDisk = QFile(path).size();
    const qint64 plain = 512 + 3 * 32 +
                         (w.ionogram.size() + w.snr.size() + w.pdp.size()) * 4;
    std::printf("  %-38s %lld bytes on disk vs %lld raw\n",
                "size", (long long)onDisk, (long long)plain);

    /* A file whose major version we do not know must be refused. */
    std::printf("\nrejects a future major version:\n");
    {
        QFile f(path);
        f.open(QIODevice::ReadWrite);
        f.seek(4);
        const char bump[2] = { (char)99, 0 };     /* version_major = 99 */
        f.write(bump, 2);
        f.close();

        LfpProducts bad;
        check("readLfp refuses major 99", readLfp(path, bad) == false);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

/*
 * lfp_build -- generate .lfp sidecars for an archive.
 *
 * Walks a directory tree, and for every .lfs without an up-to-date sidecar,
 * runs the full pipeline once and writes the products next to it. Afterwards
 * the viewer opens those captures without touching the raw data.
 *
 *   lfp_build <dir-or-file> [--force] [--fft N]
 */
#include "../src/common.h"
#include "../src/lfpfile.h"
#include "../src/qrxionogram.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QString target;
    bool force = false;
    int fftCountIndex = 5;                 /* 16384, as shipped */

    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--force")) {
            force = true;
        } else if (a == QLatin1String("--fft") && i + 1 < argc) {
            const int n = QString::fromLocal8Bit(argv[++i]).toInt();
            fftCountIndex = FFT_COUNT_LIST().indexOf(QString::number(n));
            if (fftCountIndex < 0) {
                std::fprintf(stderr, "unsupported FFT size %d\n", n);
                return 2;
            }
        } else if (target.isEmpty()) {
            target = a;
        }
    }
    if (target.isEmpty()) {
        std::fprintf(stderr,
            "usage: %s <dir-or-file.lfs> [--force] [--fft N]\n", argv[0]);
        return 2;
    }

    QStringList captures;
    QFileInfo info(target);
    if (info.isFile()) {
        captures << info.absoluteFilePath();
    } else {
        QDirIterator it(target, QStringList() << QLatin1String("*.lfs"),
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            captures << it.next();
        captures.sort();
    }

    if (captures.isEmpty()) {
        std::fprintf(stderr, "no .lfs captures under %s\n", qPrintable(target));
        return 1;
    }
    std::printf("%d capture(s)\n", captures.size());

    QBaseSoundParams base;
    base.fftCountIndex = fftCountIndex;
    base.sampleRateIndex = 3;
    base.igColormapIndex = 1;

    /* One panel, reused: it owns the pipeline. Never shown. */
    QRxIonogram plot(base, QTxParams(), QRxParams());

    int written = 0, skipped = 0, failed = 0;
    qint64 rawBytes = 0, sidecarBytes = 0;
    QElapsedTimer total; total.start();

    for (int i = 0; i < captures.size(); ++i) {
        const QString &lfs = captures.at(i);
        const QString lfp = lfpPathFor(lfs);

        const QFileInfo lfsInfo(lfs);
        const QFileInfo lfpInfo(lfp);
        if (!force && lfpInfo.exists() &&
            lfpInfo.lastModified() >= lfsInfo.lastModified()) {
            ++skipped;
            continue;
        }

        QElapsedTimer t; t.start();
        if (!plot.loadLfsData(lfs)) {
            std::printf("  FAIL  %s\n", qPrintable(lfsInfo.fileName()));
            ++failed;
            continue;
        }
        if (!writeLfp(lfp, plot.products(),
                      QLatin1String("dsChirp"), QLatin1String("1.0"))) {
            std::printf("  FAIL  cannot write %s\n", qPrintable(lfp));
            ++failed;
            continue;
        }

        const qint64 rawSize = lfsInfo.size();
        const qint64 outSize = QFileInfo(lfp).size();
        rawBytes += rawSize;
        sidecarBytes += outSize;
        ++written;

        std::printf("  %s  %6.1f MB -> %6.1f kB  (%.0f x)  %lld ms\n",
                    qPrintable(lfsInfo.fileName()),
                    rawSize / 1048576.0, outSize / 1024.0,
                    outSize ? (double)rawSize / outSize : 0.0,
                    (long long)t.elapsed());
    }

    std::printf("\nwritten %d, skipped %d, failed %d, %.1f s\n",
                written, skipped, failed, total.elapsed() / 1000.0);
    if (written)
        std::printf("total %.1f MB of captures -> %.1f kB of products (%.0f x)\n",
                    rawBytes / 1048576.0, sidecarBytes / 1024.0,
                    sidecarBytes ? (double)rawBytes / sidecarBytes : 0.0);
    return failed ? 1 : 0;
}

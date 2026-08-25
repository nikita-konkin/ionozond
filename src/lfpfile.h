#ifndef LFPFILE_H
#define LFPFILE_H

#include <QString>
#include <QVector>

/*
 * LFP -- the derived-products sidecar written next to each .lfs capture.
 * See docs/lfp-format.md for the on-disk layout and the reasoning.
 *
 * Everything the viewer needs in ~90 KB instead of re-reading 80 MB and
 * re-running 610 FFTs.
 */

struct LfpProducts {
    /* ---- identity of the source capture ---- */
    QString  txName;
    QString  rxName;
    float    txLat, txLon;
    float    rxLat, rxLon;
    qint64   startEpochMs;
    quint32  cfHz;
    quint32  rateHzS;
    quint32  sampleRateHz;
    quint32  dec;
    quint16  durS;
    quint16  whiten;
    quint32  whitenLen;
    quint32  whitenN;

    /* ---- how the products were computed ---- */
    quint32  fftCount;
    quint32  specCount;
    quint32  specPointCount;
    float    freqMinMHz, freqMaxMHz;
    float    delayMinMs, delayMaxMs;
    float    noiseGateDb;
    float    maxValueDb;
    float    lufMHz, mufMHz;
    qint32   lufIndex, mufIndex;
    quint32  tb;
    quint32  lfsrPolynomeDegree;
    bool     gated;

    /* ---- payload ---- */
    QVector<float> ionogram;   /* specCount * specPointCount, dB, row-major */
    QVector<float> snr;        /* specCount */
    QVector<float> pdp;        /* specPointCount */

    LfpProducts()
        : txLat(0), txLon(0), rxLat(0), rxLon(0), startEpochMs(0),
          cfHz(0), rateHzS(0), sampleRateHz(0), dec(0), durS(0), whiten(0),
          whitenLen(0), whitenN(0), fftCount(0), specCount(0), specPointCount(0),
          freqMinMHz(0), freqMaxMHz(0), delayMinMs(0), delayMaxMs(0),
          noiseGateDb(0), maxValueDb(0), lufMHz(-1.0f), mufMHz(-1.0f),
          lufIndex(-1), mufIndex(-1), tb(0), lfsrPolynomeDegree(0), gated(true) {}
};

/* Sidecar path for a capture: same name, .lfp instead of .lfs. */
QString lfpPathFor(const QString &lfsPath);

bool writeLfp(const QString &path, const LfpProducts &products,
              const QString &producer, const QString &producerVersion);

/*
 * Returns false if the file is missing, not an LFP, or carries a
 * version_major this build does not understand. Unknown section types are
 * skipped, per the compatibility rule in the format doc.
 */
bool readLfp(const QString &path, LfpProducts &products);

#endif /* LFPFILE_H */

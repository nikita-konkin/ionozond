#include "qrxionogram.h"

#include "igmath.h"
#include "iganalytics.h"
#include "qigcolormap.h"

#include <QFile>
#include <QFileInfo>

#include <qwt_color_map.h>
#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

QRxIonogram::QRxIonogram(const QBaseSoundParams &base,
                         const QTxParams &tx,
                         const QRxParams &rx,
                         QWidget *parent)
    : QwtPlot(parent),
      m_base(base),
      m_tx(tx),
      m_rx(rx),
      m_spectrogram(0),
      m_raster(0),
      m_data(0),
      m_specCount(0),
      m_specPointCount(0),
      m_levelMin(0.0),
      m_levelMax(0.0),
      m_isControl(false),
      m_noiseGate(true),
      m_haveHeader(false),
      m_luf(-1.0f),
      m_muf(-1.0f)
{
    setupPlot();
}

QRxIonogram::~QRxIonogram()
{
    releaseData();
}

void QRxIonogram::setupPlot()
{
    setAutoReplot(false);
    setCanvasBackground(Qt::white);

    /* Keep the panels from demanding so much height that the console pane
     * gets squeezed out of the main window. */
    setMinimumSize(220, 150);

    setAxisTitle(QwtPlot::xBottom, QwtText(QString::fromLatin1(AXIS_FREQ_TITLE)));
    setAxisTitle(QwtPlot::yLeft, QwtText(QString::fromLatin1(TIME_AXIS_TITLE_TEXT)));

    m_spectrogram = new QwtPlotSpectrogram();
    m_spectrogram->setRenderThreadCount(0);   /* use all cores */
    m_spectrogram->setColorMap(new QIgColorMap(colorLevelsForIndex(m_base.igColormapIndex)));
    m_spectrogram->attach(this);

    /* The colour bar down the right-hand side, as in the original. */
    QwtScaleWidget *rightAxis = axisWidget(QwtPlot::yRight);
    rightAxis->setColorBarEnabled(true);
    enableAxis(QwtPlot::yRight, true);

    setupDefaultAxes();
}

/*
 * Scales for an empty panel, derived from the schedule entry rather than a
 * capture, so a panel with no data yet still shows the right sweep and delay
 * window instead of Qwt's default -0.6..0.6.
 *
 * schedule.ini holds cf and rate in kHz, sample rate comes from config.ini.
 */
void QRxIonogram::setupDefaultAxes()
{
    const double sampleRateMHz = m_base.sampleRate_Mhz();
    if (sampleRateMHz <= 0.0 || m_tx.cf == 0)
        return;

    const double cfMHz = (double)m_tx.cf / 1000.0;
    const double freqStartMHz = cfMHz - sampleRateMHz / 2.0;
    const double freqStopMHz  = freqStartMHz + (double)m_tx.dur * (double)m_tx.rate / 1000.0;

    const double dKm    = earthDistanceKm(m_tx.lat, m_tx.lon, m_rx.lat, m_rx.lon);
    const double rayKm  = rayDistanceKm(dKm);
    const double rminKm = rayKm - VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;
    const double rmaxKm = rminKm + VIRT_HEIGHT_WINDOW_KM_DEFAULT
                                 + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;

    setAxisScale(QwtPlot::xBottom, freqStartMHz, freqStopMHz);
    setAxisScale(QwtPlot::yLeft, timeMsFromHeightKm(rminKm), timeMsFromHeightKm(rmaxKm));

    m_horizontal = QIgWindow(QPointF(freqStartMHz, 0.0), QPointF(freqStopMHz, 0.0));
    m_vertical   = QIgWindow(QPointF(0.0, timeMsFromHeightKm(rminKm)),
                             QPointF(0.0, timeMsFromHeightKm(rmaxKm)));
}

void QRxIonogram::releaseData()
{
    if (m_data) {
        for (int i = 0; i < m_specCount; ++i)
            delete[] m_data[i];
        delete[] m_data;
        m_data = 0;
    }
    m_specCount = 0;
    m_specPointCount = 0;
}

void QRxIonogram::setIgFilename(const QString &fileName) { m_igFileName = fileName; }
QString QRxIonogram::igFileName() const                  { return m_igFileName; }

void QRxIonogram::setStartDateTime(const QDateTime &dateTime) { m_startDateTime = dateTime; }
QDateTime QRxIonogram::startDateTime() const                  { return m_startDateTime; }

QIgWindow QRxIonogram::horizontalInterval() const { return m_horizontal; }
QIgWindow QRxIonogram::verticalInterval() const   { return m_vertical; }

void QRxIonogram::setLevelInterval(const double &min, const double &max)
{
    m_levelMin = min;
    m_levelMax = max;

    setAxisScale(QwtPlot::yRight, min, max);
    QwtScaleWidget *rightAxis = axisWidget(QwtPlot::yRight);
    rightAxis->setColorMap(QwtInterval(min, max),
                           new QIgColorMap(colorLevelsForIndex(m_base.igColormapIndex)));
    if (m_raster)
        m_raster->setInterval(Qt::ZAxis, QwtInterval(min, max));
}

void QRxIonogram::setColorMapIndex(int index)
{
    m_base.igColormapIndex = index;
    if (m_spectrogram)
        m_spectrogram->setColorMap(new QIgColorMap(colorLevelsForIndex(index)));
    if (m_levelMax > m_levelMin)
        setLevelInterval(m_levelMin, m_levelMax);
    replot();
}

void QRxIonogram::clear()
{
    releaseData();
    if (m_spectrogram)
        m_spectrogram->setData(0);
    m_raster = 0;
    replot();
}

bool QRxIonogram::loadLfsData(const QString &fileName)
{
    FILE *fp = std::fopen(QFile::encodeName(fileName).constData(), "rb");
    if (!fp)
        return false;

    lfs_header h;
    if (!lfsheader_read(fp, h)) {
        std::fclose(fp);
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long fileSize = std::ftell(fp);
    std::fseek(fp, sizeof(lfs_header), SEEK_SET);

    const int fftCount = m_base.fftCount() > 0 ? m_base.fftCount() : 16384;
    const long totalSamples = (fileSize - (long)sizeof(lfs_header)) / 8;
    const int nSpec = (int)(totalSamples / fftCount);
    if (nSpec <= 0) {
        std::fclose(fp);
        return false;
    }

    /* ---- spectra ---- */
    std::vector<std::vector<double> > storage(nSpec, std::vector<double>(fftCount));
    std::vector<double *> spec(nSpec);
    for (int i = 0; i < nSpec; ++i)
        spec[i] = storage[i].data();

    std::vector<float> window = calculateHanningWindow((unsigned)fftCount);
    double maxValue = 0.0;
    const int built = buildSpectra(fp, fftCount, window.data(), nSpec, spec.data(), &maxValue);
    std::fclose(fp);
    if (built <= 0)
        return false;

    /* ---- axes from the header ---- */
    const double ifRate = (double)h.sample_rate / (double)h.dec;
    const double freqStartMHz = ((double)h.cf - (double)h.sample_rate / 2.0) / HZ_IN_MHZ;
    const double freqStopMHz  = freqStartMHz + (double)h.dur * (double)h.rate / HZ_IN_MHZ;

    const double dKm    = earthDistanceKm(h.tx_latitude, h.tx_longitude,
                                          h.rx_latitude, h.rx_longitude);
    const double rayKm  = rayDistanceKm(dKm);
    const double rminKm = rayKm - VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;
    const double rmaxKm = rminKm + VIRT_HEIGHT_WINDOW_KM_DEFAULT
                                 + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;

    /* Rows ascend with delay -- see the note in igmath.h. */
    const double hMax = LIGHT_SPEED_KM_S * (ifRate / 2.0) / (double)h.rate;
    const double hMin = -hMax;
    const double kmPerRow = (hMax - hMin) / (double)(fftCount - 1);

    const int rowLow  = std::max(0, (int)std::floor((rminKm - hMin) / kmPerRow));
    const int rowHigh = std::min(fftCount - 1, (int)std::ceil((rmaxKm - hMin) / kmPerRow));
    const int rows = rowHigh - rowLow + 1;
    if (rows <= 1)
        return false;

    /* ---- windowed dB array, ascending in delay ---- */
    releaseData();
    m_specCount = built;
    m_specPointCount = rows;
    m_data = new float *[m_specCount];

    double winMax = 0.0;
    std::vector<float> fullDb(fftCount);

    for (int c = 0; c < m_specCount; ++c) {
        m_data[c] = new float[rows];

        /*
         * The noise gate must see the WHOLE spectrum, not just the delay
         * window: the threshold comes from the shape of the noise
         * distribution, and 231 windowed rows do not sample it well enough.
         * QIonogram likewise works on the full array and crops afterwards.
         */
        for (int k = 0; k < fftCount; ++k) {
            const double p = spec[c][k];
            fullDb[k] = (float)(10.0 * std::log10(p > 0.0 ? p : 1e-300));
        }

        float limit = 0.0f;
        if (m_noiseGate)
            limit = getPowerDynamicLimit(fullDb.data(), fftCount);

        for (int r = 0; r < rows; ++r) {
            const double p = spec[c][rowLow + r];
            const float db = fullDb[rowLow + r];
            m_data[c][r] = (limit > db) ? 0.0f : db;
            if (p > winMax && !(limit > db))
                winMax = p;
        }
    }

    m_horizontal = QIgWindow(QPointF(freqStartMHz, 0.0),
                             QPointF(freqStopMHz, 0.0));
    m_vertical = QIgWindow(QPointF(0.0, timeMsFromHeightKm(hMin + rowLow * kmPerRow)),
                           QPointF(0.0, timeMsFromHeightKm(hMin + rowHigh * kmPerRow)));

    const double tMin = timeMsFromHeightKm(hMin + rowLow * kmPerRow);
    const double tMax = timeMsFromHeightKm(hMin + rowHigh * kmPerRow);

    const QRectF area(freqStartMHz, tMin, freqStopMHz - freqStartMHz, tMax - tMin);

    /*
     * Scale to the maximum inside the display window. Using the frame-wide
     * maximum (as setLevelInterval(0.125*maxDb, maxDb) implies) washes the
     * panel out whenever the direct signal falls outside the window; the
     * original compensates in the QIonogram post-processing that is not
     * implemented yet. See NOTES.md.
     */
    const double topDb = 10.0 * std::log10(winMax > 0.0 ? winMax : 1.0);

    m_raster = new RasterData(m_data, m_specCount, m_specPointCount, area, (float)topDb);
    m_spectrogram->setData(m_raster);

    setAxisScale(QwtPlot::xBottom, freqStartMHz, freqStopMHz);
    setAxisScale(QwtPlot::yLeft, tMin, tMax);
    setLevelInterval(0.125 * topDb, topDb);

    /*
     * ---- derived products ----
     *
     * The SNR sum works on the LINEAR normalised power, not the dB array: the
     * spectra were divided by their own median, so the median of every
     * spectrum is 1 by construction and the noise estimate reduces to
     * 1 * 2*ln(2). The gate decides which points contribute.
     */
    {
        std::vector<const float *> gated(m_specCount);
        for (int c = 0; c < m_specCount; ++c)
            gated[c] = m_data[c];

        if (m_noiseGate) {
            /* Second, statistical gate, after the per-spectrum Rosin threshold. */
            deleteObjectsUnderNoiseLevel(m_data, gated.data(),
                                         m_specCount, m_specPointCount,
                                         (float)noiseLevelDbForNormalisedSpectra());

            /*
             * Speckle filter: keep only points with enough neighbours inside a
             * 9x3 window. Without this the band-edge detector below latches on
             * to isolated noise and reports the whole sweep as usable.
             */
            std::vector<std::vector<float> > before(m_specCount);
            std::vector<const float *> beforeRows(m_specCount);
            for (int c = 0; c < m_specCount; ++c) {
                before[c].assign(m_data[c], m_data[c] + m_specPointCount);
                beforeRows[c] = before[c].data();
            }
            deleteSmallObjects(m_data, beforeRows.data(),
                               m_specCount, m_specPointCount,
                               m_base.objSizeH, m_base.objSizeV, m_base.objLevel);
        }

        const UsageFrequencies uf =
            calculateUsageFrequencies(gated.data(), m_specCount, m_specPointCount);

        std::vector<double> medians(m_specCount, 1.0);   /* normalised */

        /* Sum linear power over the points the gate let through. */
        QVector<float> snr;
        snr.reserve(m_specCount);
        const double NOISE_FACTOR = 1.3862943611198906;  /* 2*ln(2) */
        for (int c = 0; c < m_specCount; ++c) {
            float value = 0.0f;
            if (uf.isValid() && c >= uf.lufIndex && c <= uf.mufIndex) {
                double sum = 0.0;
                for (int r = 0; r < m_specPointCount; ++r)
                    if (m_data[c][r] > 0.0f)
                        sum += spec[c][rowLow + r];
                const double ratio = sum / (medians[c] * NOISE_FACTOR) - 1.0;
                if (ratio >= 1.0)
                    value = (float)(10.0 * std::log10(ratio));
            }
            snr.append(value);
        }
        m_snrVector = snr;

        const double fSpan = freqStopMHz - freqStartMHz;
        m_luf = uf.isValid() && m_specCount > 1
                    ? (float)(freqStartMHz + fSpan * uf.lufIndex / (m_specCount - 1))
                    : -1.0f;
        m_muf = uf.isValid() && m_specCount > 1
                    ? (float)(freqStartMHz + fSpan * uf.mufIndex / (m_specCount - 1))
                    : -1.0f;

        m_pdpVector = calcIntegratedPowerDelayProfile(
            gated.data(), m_specCount, m_specPointCount, tMin, tMax, uf);
    }

    m_header = h;
    m_haveHeader = true;

    m_igFileName = fileName;
    m_startDateTime = QDateTime(QDate(h.start_year, h.start_month, h.start_day),
                                QTime(h.start_hour, h.start_minute, h.start_second),
                                Qt::UTC);
    setTitle(QwtText(m_startDateTime.toString(QLatin1String("dd MMM hh:mm:ss"))));

    replot();

    emit rxIonogramBuilt(m_igFileName, (unsigned int)m_specPointCount,
                         m_horizontal, m_vertical,
                         (unsigned int)m_base.igVerticalScaleIndex, m_isControl);
    return true;
}

/* ------------------------------------------------------------------ *
 * Sidecar support (see docs/lfp-format.md)
 * ------------------------------------------------------------------ */

LfpProducts QRxIonogram::products() const
{
    LfpProducts p;

    if (m_haveHeader) {
        p.txName = QString::fromLatin1(m_header.tx_name,
                       qstrnlen(m_header.tx_name, sizeof(m_header.tx_name)));
        p.rxName = QString::fromLatin1(m_header.rx_name,
                       qstrnlen(m_header.rx_name, sizeof(m_header.rx_name)));
        p.txLat = m_header.tx_latitude;  p.txLon = m_header.tx_longitude;
        p.rxLat = m_header.rx_latitude;  p.rxLon = m_header.rx_longitude;
        p.cfHz = m_header.cf;
        p.rateHzS = m_header.rate;
        p.sampleRateHz = m_header.sample_rate;
        p.dec = m_header.dec;
        p.durS = m_header.dur;
        p.whiten = m_header.whiten;
        p.whitenLen = m_header.whiten_len;
        p.whitenN = m_header.whiten_n;
    } else {
        p.txName = m_tx.name;
        p.rxName = m_rx.name;
        p.txLat = (float)m_tx.lat;  p.txLon = (float)m_tx.lon;
        p.rxLat = (float)m_rx.lat;  p.rxLon = (float)m_rx.lon;
    }
    p.startEpochMs = m_startDateTime.toMSecsSinceEpoch();

    p.fftCount       = (quint32)(m_base.fftCount() > 0 ? m_base.fftCount() : 16384);
    p.specCount      = (quint32)m_specCount;
    p.specPointCount = (quint32)m_specPointCount;

    p.freqMinMHz = (float)m_horizontal.left();
    p.freqMaxMHz = (float)m_horizontal.right();
    p.delayMinMs = (float)m_vertical.top();
    p.delayMaxMs = (float)m_vertical.bottom();
    p.maxValueDb = (float)m_levelMax;
    p.lufMHz = m_luf;
    p.mufMHz = m_muf;
    p.gated = m_noiseGate;

    /* Flatten the ionogram row-major by spectrum. */
    p.ionogram.reserve(m_specCount * m_specPointCount);
    for (int c = 0; c < m_specCount; ++c)
        for (int r = 0; r < m_specPointCount; ++r)
            p.ionogram.append(m_data[c][r]);

    p.snr = m_snrVector;

    p.pdp.reserve(m_pdpVector.size());
    for (int i = 0; i < m_pdpVector.size(); ++i)
        p.pdp.append((float)m_pdpVector.at(i).value);

    return p;
}

bool QRxIonogram::applyProducts(const LfpProducts &p)
{
    if (p.specCount == 0 || p.specPointCount == 0)
        return false;
    if ((quint32)p.ionogram.size() != p.specCount * p.specPointCount)
        return false;

    releaseData();
    m_specCount = (int)p.specCount;
    m_specPointCount = (int)p.specPointCount;
    m_data = new float *[m_specCount];

    double top = 0.0;
    for (int c = 0; c < m_specCount; ++c) {
        m_data[c] = new float[m_specPointCount];
        for (int r = 0; r < m_specPointCount; ++r) {
            const float v = p.ionogram.at(c * m_specPointCount + r);
            m_data[c][r] = v;
            if (v > top) top = v;
        }
    }

    m_horizontal = QIgWindow(QPointF(p.freqMinMHz, 0.0), QPointF(p.freqMaxMHz, 0.0));
    m_vertical   = QIgWindow(QPointF(0.0, p.delayMinMs), QPointF(0.0, p.delayMaxMs));
    m_luf = p.lufMHz;
    m_muf = p.mufMHz;
    m_snrVector = p.snr;

    /* Rebuild the interval samples from the stored values and the axis. */
    m_pdpVector.clear();
    if (p.pdp.size() > 1) {
        const double step = (p.delayMaxMs - p.delayMinMs) / (double)(p.pdp.size() - 1);
        const double half = step * 0.5;
        for (int i = 0; i < p.pdp.size(); ++i) {
            const double y = p.delayMinMs + step * i;
            m_pdpVector.append(QwtIntervalSample(p.pdp.at(i),
                                                 QwtInterval(y - half, y + half)));
        }
    }

    const QRectF area(p.freqMinMHz, p.delayMinMs,
                      p.freqMaxMHz - p.freqMinMHz, p.delayMaxMs - p.delayMinMs);
    if (top <= 0.0)
        top = 1.0;

    m_raster = new RasterData(m_data, m_specCount, m_specPointCount, area, (float)top);
    m_spectrogram->setData(m_raster);

    setAxisScale(QwtPlot::xBottom, p.freqMinMHz, p.freqMaxMHz);
    setAxisScale(QwtPlot::yLeft, p.delayMinMs, p.delayMaxMs);
    setLevelInterval(0.125 * top, top);

    m_startDateTime = QDateTime::fromMSecsSinceEpoch(p.startEpochMs, Qt::UTC);
    setTitle(QwtText(m_startDateTime.toString(QLatin1String("dd MMM hh:mm:ss"))));

    replot();
    return true;
}

bool QRxIonogram::load(const QString &lfsPath, bool useSidecar, bool writeSidecar)
{
    const QString sidecar = lfpPathFor(lfsPath);

    if (useSidecar && QFile::exists(sidecar)) {
        LfpProducts p;
        if (readLfp(sidecar, p) && applyProducts(p)) {
            m_igFileName = lfsPath;
            emit rxIonogramBuilt(m_igFileName, (unsigned int)m_specPointCount,
                                 m_horizontal, m_vertical,
                                 (unsigned int)m_base.igVerticalScaleIndex,
                                 m_isControl);
            return true;
        }
        /* A stale or unreadable sidecar just means doing it the slow way. */
    }

    if (!loadLfsData(lfsPath))
        return false;

    if (writeSidecar)
        writeLfp(sidecar, products(), QLatin1String("dsChirp"), QLatin1String("1.0"));

    return true;
}

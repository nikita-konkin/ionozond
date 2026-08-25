#ifndef QRXIONOGRAM_H
#define QRXIONOGRAM_H

#include "common.h"
#include "lfs_header.h"
#include "lfpfile.h"
#include "rasterdata.h"

#include <QDateTime>
#include <QString>
#include <QVector>

#include <qwt_plot.h>
#include <qwt_plot_spectrogram.h>

/*
 * QRxIonogram @0x46dc50 -- one ionogram panel.
 *
 * The original polls the capture while the sounder is still writing it
 * (timerEvent -> readNewLfsSpecData over the newly arrived bytes). This
 * version loads a complete capture in one pass, which covers viewing the
 * archive; incremental loading is still to do.
 *
 * moc signal:
 *   rxIonogramBuilt(igFileName, specPointCount, igWindow, igVisibleWindow,
 *                   yTypeId, isControlIonogram)
 */
class QRxIonogram : public QwtPlot
{
    Q_OBJECT

public:
    QRxIonogram(const QBaseSoundParams &base,
                const QTxParams &tx,
                const QRxParams &rx,
                QWidget *parent = 0);
    ~QRxIonogram();

    void    setIgFilename(const QString &fileName);
    QString igFileName() const;

    void      setStartDateTime(const QDateTime &dateTime);
    QDateTime startDateTime() const;

    /* Frequency sweep and delay window, both derived from the capture header. */
    QIgWindow horizontalInterval() const;
    QIgWindow verticalInterval() const;

    void setLevelInterval(const double &min, const double &max);
    void clear();

    /* Switch colour map at runtime (used by the viewer harness). */
    void setColorMapIndex(int index);

    /* Per-spectrum Rosin noise gate (QIonogram's power-limit stage). On by
     * default; the viewer exposes a toggle so the raw data can be seen too. */
    bool noiseGate() const        { return m_noiseGate; }
    void setNoiseGate(bool on)    { m_noiseGate = on; }

    /* Load a complete capture and build the panel. Returns false if the file
     * is missing or not a valid .lfs. */
    bool loadLfsData(const QString &fileName);

    /*
     * Preferred entry point: use the .lfp sidecar when one is present, which
     * skips re-reading 80 MB and re-running the FFTs. Falls back to the
     * capture, and writes a sidecar for next time when asked.
     */
    bool load(const QString &lfsPath, bool useSidecar = true, bool writeSidecar = true);

    /* The derived products, for writing a sidecar. */
    LfpProducts products() const;
    /* Rebuild the panel straight from products, with no signal processing. */
    bool applyProducts(const LfpProducts &p);

    /* Derived products, valid after a successful loadLfsData(). */
    const QVector<float> &snrVector() const { return m_snrVector; }
    const PdpVector      &pdpVector() const { return m_pdpVector; }
    float luf_MHz() const { return m_luf; }
    float muf_MHz() const { return m_muf; }

    bool isControlIonogram() const  { return m_isControl; }
    void setControlIonogram(bool v) { m_isControl = v; }

signals:
    void rxIonogramBuilt(const QString &igFileName,
                         const unsigned int &specPointCount,
                         const QIgWindow &igWindow,
                         const QIgWindow &igVisibleWindow,
                         const unsigned int &yTypeId,
                         const bool &isControlIonogram);

private:
    void setupPlot();
    void setupDefaultAxes();
    void releaseData();

    QBaseSoundParams m_base;
    QTxParams        m_tx;
    QRxParams        m_rx;

    QwtPlotSpectrogram *m_spectrogram;
    RasterData         *m_raster;

    /* Windowed dB values, [spectrum][delay row], ascending in delay. */
    float **m_data;
    int     m_specCount;
    int     m_specPointCount;

    QString   m_igFileName;
    QDateTime m_startDateTime;
    QIgWindow m_horizontal;   /* MHz */
    QIgWindow m_vertical;     /* ms  */
    double    m_levelMin;
    double    m_levelMax;
    bool      m_isControl;
    bool      m_noiseGate;

    /* Header of the capture last loaded; the station identity in the
     * sidecar comes from here, not from the schedule, so a sidecar is
     * self-describing even when built by a tool with no schedule. */
    lfs_header     m_header;
    bool           m_haveHeader;

    QVector<float> m_snrVector;
    PdpVector      m_pdpVector;
    float          m_luf;
    float          m_muf;
};

#endif /* QRXIONOGRAM_H */

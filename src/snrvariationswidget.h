#ifndef SNRVARIATIONSWIDGET_H
#define SNRVARIATIONSWIDGET_H

#include "common.h"
#include "qigcolormap.h"

#include <QDateTime>
#include <QVector>

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_spectrogram.h>
#include <qwt_raster_data.h>

/*
 * SnrVariationsWidget @0x482e10 -- the "Сигнал/шум" panel.
 *
 * Daily course of the signal/noise ratio: time along x, frequency up y,
 * colour = SNR in dB, with the LUF and MUF traced over the top.
 *
 * Original ctor:
 *   SnrVariationsWidget(uint periodHour, uint timeBandMin, float freqBand,
 *                       float freqMin, float freqMax,
 *                       QVector<QColorLevel> const&, bool autoMax, float max,
 *                       QWidget*)
 *
 * moc signals: valueAdded(time, SnrVector, luf_Mhz, muf_Mhz),
 *              firstTimeBandRemoved(), lufVectorChanged(QVector<float>),
 *              mufVectorChanged(QVector<float>), axisFreqMin/MaxValueChanged(),
 *              axisScaleChanged(axisId, min, max)
 * moc slots:   onValueAdded(...), onFirstTimeBandRemoved(),
 *              onLufVectorChanged(...), onMufVectorChanged(...), ...
 */
class SnrVariationsWidget : public QwtPlot
{
    Q_OBJECT

public:
    SnrVariationsWidget(unsigned int periodHour,
                        unsigned int timeBandMin,
                        float freqMin_MHz,
                        float freqMax_MHz,
                        const QVector<QColorLevel> &colorLevels,
                        bool autoMax,
                        float maxValue,
                        QWidget *parent = 0);
    ~SnrVariationsWidget();

signals:
    void valueAdded(const QDateTime &time, const SnrVector &snrVector,
                    const float &luf_Mhz, const float &muf_Mhz);
    void firstTimeBandRemoved();

public slots:
    /* One sounding's worth of data: its SNR spectrum plus the band edges. */
    void onValueAdded(const QDateTime &time, const SnrVector &snrVector,
                      const float &luf_Mhz, const float &muf_Mhz);
    void onFirstTimeBandRemoved();
    void clear();

private:
    void rebuild();
    void dropExpired();

    class SnrRaster;

    unsigned int m_periodHour;
    unsigned int m_timeBandMin;
    float        m_freqMin;
    float        m_freqMax;
    bool         m_autoMax;
    float        m_maxValue;

    QVector<QColorLevel> m_colorLevels;

    QVector<QDateTime>  m_times;
    QVector<SnrVector>  m_columns;
    QVector<float>      m_lufs;
    QVector<float>      m_mufs;

    QwtPlotSpectrogram *m_spectrogram;
    SnrRaster          *m_raster;
    QwtPlotCurve       *m_lufCurve;
    QwtPlotCurve       *m_mufCurve;
};

#endif /* SNRVARIATIONSWIDGET_H */

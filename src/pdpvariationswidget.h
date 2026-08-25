#ifndef PDPVARIATIONSWIDGET_H
#define PDPVARIATIONSWIDGET_H

#include "common.h"
#include "qigcolormap.h"

#include <QDateTime>
#include <QVector>

#include <qwt_plot.h>
#include <qwt_plot_spectrogram.h>
#include <qwt_raster_data.h>

/*
 * PdpVariationsWidget @0x486700 -- the "ПЗМ" panel, профиль задержки мощности.
 *
 * Daily course of the power-delay profile: time along x, delay up y, colour is
 * the integrated power.
 *
 * Original ctor:
 *   PdpVariationsWidget(uint periodHour, uint timeBandMin,
 *                       float delayMin, float delayMax,
 *                       QVector<QColorLevel> const&, bool autoMax, float max,
 *                       QWidget*)
 *
 * moc signals: valueAdded(time, PdpVector), firstTimeBandRemoved(),
 *              signalDelayVectorsChanged(minDelays, maxDelays)
 */
class PdpVariationsWidget : public QwtPlot
{
    Q_OBJECT

public:
    PdpVariationsWidget(unsigned int periodHour,
                        unsigned int timeBandMin,
                        float delayMin_ms,
                        float delayMax_ms,
                        const QVector<QColorLevel> &colorLevels,
                        bool autoMax,
                        float maxValue,
                        QWidget *parent = 0);
    ~PdpVariationsWidget();

signals:
    void valueAdded(const QDateTime &time, const PdpVector &pdpVector);
    void firstTimeBandRemoved();
    void signalDelayVectorsChanged(const QVector<double> &minDelays,
                                   const QVector<double> &maxDelays);

public slots:
    void onValueAdded(const QDateTime &time, const PdpVector &pdpVector);
    void onFirstTimeBandRemoved();
    void clear();

private:
    void rebuild();
    void dropExpired();

    class PdpRaster;

    unsigned int m_periodHour;
    unsigned int m_timeBandMin;
    float        m_delayMin;
    float        m_delayMax;
    bool         m_autoMax;
    float        m_maxValue;

    QVector<QColorLevel> m_colorLevels;

    QVector<QDateTime> m_times;
    QVector<PdpVector> m_columns;

    QwtPlotSpectrogram *m_spectrogram;
    PdpRaster          *m_raster;
};

#endif /* PDPVARIATIONSWIDGET_H */

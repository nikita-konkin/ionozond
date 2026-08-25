#include "pdpvariationswidget.h"

#include "datetimescaledraw.h"

#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <algorithm>

/*
 * PdpVariationsRasterData @0x489620 -- x is time (seconds since epoch),
 * y is delay in ms, the value is the integrated power at that delay.
 */
class PdpVariationsWidget::PdpRaster : public QwtRasterData
{
public:
    PdpRaster(const QVector<QDateTime> *times, const QVector<PdpVector> *columns,
              float delayMin, float delayMax)
        : m_times(times), m_columns(columns),
          m_delayMin(delayMin), m_delayMax(delayMax) {}

    virtual double value(double x, double y) const
    {
        if (!m_times || m_times->isEmpty() || m_columns->isEmpty())
            return 0.0;

        const double t0 = (double)m_times->first().toMSecsSinceEpoch() / 1000.0;
        const double t1 = (double)m_times->last().toMSecsSinceEpoch() / 1000.0;
        const double span = (t1 > t0) ? (t1 - t0) : 1.0;

        int col = (int)((x - t0) / span * (m_columns->size() - 1) + 0.5);
        col = std::min(std::max(col, 0), m_columns->size() - 1);

        const PdpVector &v = m_columns->at(col);
        if (v.isEmpty())
            return 0.0;

        int row = (int)((y - m_delayMin) / (m_delayMax - m_delayMin) * (v.size() - 1) + 0.5);
        row = std::min(std::max(row, 0), v.size() - 1);
        return v.at(row).value;
    }

private:
    const QVector<QDateTime> *m_times;
    const QVector<PdpVector> *m_columns;
    float m_delayMin;
    float m_delayMax;
};

PdpVariationsWidget::PdpVariationsWidget(unsigned int periodHour,
                                         unsigned int timeBandMin,
                                         float delayMin_ms,
                                         float delayMax_ms,
                                         const QVector<QColorLevel> &colorLevels,
                                         bool autoMax,
                                         float maxValue,
                                         QWidget *parent)
    : QwtPlot(parent),
      m_periodHour(periodHour),
      m_timeBandMin(timeBandMin),
      m_delayMin(delayMin_ms),
      m_delayMax(delayMax_ms),
      m_autoMax(autoMax),
      m_maxValue(maxValue),
      m_colorLevels(colorLevels),
      m_spectrogram(0),
      m_raster(0)
{
    setAutoReplot(false);
    setCanvasBackground(Qt::white);
    setMinimumSize(220, 150);

    setAxisTitle(QwtPlot::yLeft, QwtText(QString::fromLatin1(DELAY_AXIS_TITLE_TEXT)));
    setAxisTitle(QwtPlot::xBottom, QwtText(QLatin1String("UTC")));
    setAxisScaleDraw(QwtPlot::xBottom, new DateTimeScaleDraw(QLatin1String("dd MMM")));
    setAxisScale(QwtPlot::yLeft, m_delayMin, m_delayMax);

    m_raster = new PdpRaster(&m_times, &m_columns, m_delayMin, m_delayMax);
    m_spectrogram = new QwtPlotSpectrogram();
    m_spectrogram->setRenderThreadCount(0);
    m_spectrogram->setColorMap(new QIgColorMap(m_colorLevels));
    m_spectrogram->setData(m_raster);
    m_spectrogram->attach(this);

    QwtScaleWidget *rightAxis = axisWidget(QwtPlot::yRight);
    rightAxis->setColorBarEnabled(true);
    enableAxis(QwtPlot::yRight, true);
}

PdpVariationsWidget::~PdpVariationsWidget()
{
}

void PdpVariationsWidget::onValueAdded(const QDateTime &time, const PdpVector &pdpVector)
{
    m_times.append(time);
    m_columns.append(pdpVector);

    dropExpired();
    rebuild();

    emit valueAdded(time, pdpVector);
}

/* Discard anything older than the configured period. */
void PdpVariationsWidget::dropExpired()
{
    if (m_times.isEmpty() || m_periodHour == 0)
        return;

    const QDateTime cutoff = m_times.last().addSecs(-(qint64)m_periodHour * 3600);
    bool removed = false;
    while (m_times.size() > 1 && m_times.first() < cutoff) {
        m_times.removeFirst();
        m_columns.removeFirst();
        removed = true;
    }
    if (removed)
        emit firstTimeBandRemoved();
}

void PdpVariationsWidget::onFirstTimeBandRemoved()
{
    dropExpired();
    rebuild();
}

void PdpVariationsWidget::clear()
{
    m_times.clear();
    m_columns.clear();
    rebuild();
}

void PdpVariationsWidget::rebuild()
{
    if (m_times.isEmpty()) {
        replot();
        return;
    }

    const double t0 = (double)m_times.first().toMSecsSinceEpoch() / 1000.0;
    double t1 = (double)m_times.last().toMSecsSinceEpoch() / 1000.0;
    if (t1 <= t0)
        t1 = t0 + 1.0;

    double top = m_maxValue;
    if (m_autoMax || top <= 0.0) {
        top = 0.0;
        for (int c = 0; c < m_columns.size(); ++c)
            for (int i = 0; i < m_columns.at(c).size(); ++i)
                top = std::max(top, m_columns.at(c).at(i).value);
        if (top <= 0.0)
            top = 1.0;
    }

    m_raster->setInterval(Qt::XAxis, QwtInterval(t0, t1));
    m_raster->setInterval(Qt::YAxis, QwtInterval(m_delayMin, m_delayMax));
    m_raster->setInterval(Qt::ZAxis, QwtInterval(0.0, top));

    setAxisScale(QwtPlot::xBottom, t0, t1);
    setAxisScale(QwtPlot::yRight, 0.0, top);
    axisWidget(QwtPlot::yRight)->setColorMap(QwtInterval(0.0, top),
                                             new QIgColorMap(m_colorLevels));

    /*
     * Envelope of the signal delay: the first and last bin carrying power in
     * each column. The original reports this through
     * signalDelayVectorsChanged() for whoever draws the bounding curves.
     */
    QVector<double> minDelays, maxDelays;
    for (int c = 0; c < m_columns.size(); ++c) {
        const PdpVector &v = m_columns.at(c);
        double lo = m_delayMax, hi = m_delayMin;
        for (int i = 0; i < v.size(); ++i) {
            if (v.at(i).value > 0.0) {
                lo = std::min(lo, v.at(i).interval.minValue());
                hi = std::max(hi, v.at(i).interval.maxValue());
            }
        }
        minDelays.append(lo);
        maxDelays.append(hi);
    }
    emit signalDelayVectorsChanged(minDelays, maxDelays);

    replot();
}

#include "snrvariationswidget.h"

#include "datetimescaledraw.h"

#include <qwt_scale_widget.h>
#include <qwt_text.h>

#include <algorithm>

/*
 * SnrVariationsRasterData @0x485990 -- x is time (seconds since epoch),
 * y is frequency in MHz, the value is SNR in dB.
 */
class SnrVariationsWidget::SnrRaster : public QwtRasterData
{
public:
    SnrRaster(const QVector<QDateTime> *times,
              const QVector<SnrVector> *columns,
              float freqMin, float freqMax)
        : m_times(times), m_columns(columns),
          m_freqMin(freqMin), m_freqMax(freqMax) {}

    virtual double value(double x, double y) const
    {
        if (!m_times || m_times->isEmpty() || m_columns->isEmpty())
            return 0.0;

        /* nearest column in time */
        const double t0 = (double)m_times->first().toMSecsSinceEpoch() / 1000.0;
        const double t1 = (double)m_times->last().toMSecsSinceEpoch() / 1000.0;
        const double span = (t1 > t0) ? (t1 - t0) : 1.0;

        int col = (int)((x - t0) / span * (m_columns->size() - 1) + 0.5);
        col = std::min(std::max(col, 0), m_columns->size() - 1);

        const SnrVector &v = m_columns->at(col);
        if (v.isEmpty())
            return 0.0;

        int row = (int)((y - m_freqMin) / (m_freqMax - m_freqMin) * (v.size() - 1) + 0.5);
        row = std::min(std::max(row, 0), v.size() - 1);
        return v.at(row);
    }

private:
    const QVector<QDateTime> *m_times;
    const QVector<SnrVector> *m_columns;
    float m_freqMin;
    float m_freqMax;
};

SnrVariationsWidget::SnrVariationsWidget(unsigned int periodHour,
                                         unsigned int timeBandMin,
                                         float freqMin_MHz,
                                         float freqMax_MHz,
                                         const QVector<QColorLevel> &colorLevels,
                                         bool autoMax,
                                         float maxValue,
                                         QWidget *parent)
    : QwtPlot(parent),
      m_periodHour(periodHour),
      m_timeBandMin(timeBandMin),
      m_freqMin(freqMin_MHz),
      m_freqMax(freqMax_MHz),
      m_autoMax(autoMax),
      m_maxValue(maxValue),
      m_colorLevels(colorLevels),
      m_spectrogram(0),
      m_raster(0),
      m_lufCurve(0),
      m_mufCurve(0)
{
    setAutoReplot(false);
    setCanvasBackground(Qt::white);
    setMinimumSize(220, 150);

    setAxisTitle(QwtPlot::yLeft, QwtText(QString::fromLatin1(AXIS_FREQ_TITLE)));
    setAxisTitle(QwtPlot::xBottom, QwtText(QLatin1String("UTC")));
    /* Empty format: the label picks its unit from the span on screen,
     * so an hour of soundings shows times rather than one repeated date. */
    setAxisScaleDraw(QwtPlot::xBottom, new DateTimeScaleDraw());
    setAxisScale(QwtPlot::yLeft, m_freqMin, m_freqMax);

    m_raster = new SnrRaster(&m_times, &m_columns, m_freqMin, m_freqMax);
    m_spectrogram = new QwtPlotSpectrogram();
    m_spectrogram->setRenderThreadCount(0);
    m_spectrogram->setColorMap(new QIgColorMap(m_colorLevels));
    m_spectrogram->setData(m_raster);
    m_spectrogram->attach(this);

    /* LUF and MUF drawn over the raster. */
    m_lufCurve = new QwtPlotCurve();
    m_lufCurve->setPen(Qt::black, 1.0);
    m_lufCurve->attach(this);

    m_mufCurve = new QwtPlotCurve();
    m_mufCurve->setPen(Qt::black, 1.0, Qt::DashLine);
    m_mufCurve->attach(this);

    QwtScaleWidget *rightAxis = axisWidget(QwtPlot::yRight);
    rightAxis->setColorBarEnabled(true);
    enableAxis(QwtPlot::yRight, true);
}

SnrVariationsWidget::~SnrVariationsWidget()
{
}

void SnrVariationsWidget::onValueAdded(const QDateTime &time, const SnrVector &snrVector,
                                       const float &luf_Mhz, const float &muf_Mhz)
{
    m_times.append(time);
    m_columns.append(snrVector);
    m_lufs.append(luf_Mhz);
    m_mufs.append(muf_Mhz);

    dropExpired();
    rebuild();

    emit valueAdded(time, snrVector, luf_Mhz, muf_Mhz);
}

/* Discard anything older than the configured period. */
void SnrVariationsWidget::dropExpired()
{
    if (m_times.isEmpty() || m_periodHour == 0)
        return;

    const QDateTime cutoff = m_times.last().addSecs(-(qint64)m_periodHour * 3600);
    bool removed = false;
    while (m_times.size() > 1 && m_times.first() < cutoff) {
        m_times.removeFirst();
        m_columns.removeFirst();
        m_lufs.removeFirst();
        m_mufs.removeFirst();
        removed = true;
    }
    if (removed)
        emit firstTimeBandRemoved();
}

void SnrVariationsWidget::onFirstTimeBandRemoved()
{
    dropExpired();
    rebuild();
}

void SnrVariationsWidget::clear()
{
    m_times.clear();
    m_columns.clear();
    m_lufs.clear();
    m_mufs.clear();
    rebuild();
}

void SnrVariationsWidget::rebuild()
{
    if (m_times.isEmpty()) {
        replot();
        return;
    }

    const double t0 = (double)m_times.first().toMSecsSinceEpoch() / 1000.0;
    double t1 = (double)m_times.last().toMSecsSinceEpoch() / 1000.0;
    if (t1 <= t0)
        t1 = t0 + 1.0;

    /* Colour scale: the largest SNR seen, or the configured ceiling. */
    double top = m_maxValue;
    if (m_autoMax || top <= 0.0) {
        top = 0.0;
        for (int c = 0; c < m_columns.size(); ++c)
            for (int i = 0; i < m_columns.at(c).size(); ++i)
                top = std::max(top, (double)m_columns.at(c).at(i));
        if (top <= 0.0)
            top = 1.0;
    }

    m_raster->setInterval(Qt::XAxis, QwtInterval(t0, t1));
    m_raster->setInterval(Qt::YAxis, QwtInterval(m_freqMin, m_freqMax));
    m_raster->setInterval(Qt::ZAxis, QwtInterval(0.0, top));

    setAxisScale(QwtPlot::xBottom, t0, t1);
    setAxisScale(QwtPlot::yRight, 0.0, top);
    axisWidget(QwtPlot::yRight)->setColorMap(QwtInterval(0.0, top),
                                             new QIgColorMap(m_colorLevels));

    QVector<double> x, luf, muf;
    for (int i = 0; i < m_times.size(); ++i) {
        x.append((double)m_times.at(i).toMSecsSinceEpoch() / 1000.0);
        luf.append(m_lufs.at(i));
        muf.append(m_mufs.at(i));
    }
    m_lufCurve->setSamples(x, luf);
    m_mufCurve->setSamples(x, muf);

    replot();
}

#include "qcpuusagewidget.h"

#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <qwt_plot_grid.h>
#include <qwt_scale_draw.h>
#include <qwt_text.h>

namespace {
const int HISTORY_SECONDS = 60;
}

QCpuUsageWidget::QCpuUsageWidget(QWidget *parent)
    : QwtPlot(parent),
      m_totalCurve(0),
      m_appCurve(0),
      m_lastTotal(0),
      m_lastIdle(0),
      m_coreCount(1),
      m_timer(0)
{
    m_coreCount = readCoreCount();

    setAutoReplot(false);
    setCanvasBackground(Qt::black);
    setMinimumHeight(120);
    setMaximumHeight(170);

    QFont axisFont = font();
    axisFont.setPointSizeF(qMax(6.0, axisFont.pointSizeF() - 2.0));
    setAxisFont(QwtPlot::yLeft, axisFont);
    setAxisFont(QwtPlot::xBottom, axisFont);

    /* Dark panel with dashed grey grid, as in the original. */
    QwtPlotGrid *grid = new QwtPlotGrid();
    grid->setPen(QColor(120, 120, 120), 0.0, Qt::DashLine);
    grid->attach(this);

    setAxisTitle(QwtPlot::yLeft, QwtText(QLatin1String("%")));
    setAxisTitle(QwtPlot::xBottom, QwtText(QLatin1String("t, c")));
    setAxisScale(QwtPlot::yLeft, 0.0, 100.0, 20.0);
    /* x runs backwards: 60 s ago on the left, now on the right. */
    setAxisScale(QwtPlot::xBottom, HISTORY_SECONDS, 0.0, 10.0);

    m_totalCurve = new QwtPlotCurve();
    m_totalCurve->setPen(Qt::red, 1.0);
    m_totalCurve->attach(this);

    m_appCurve = new QwtPlotCurve();
    m_appCurve->setPen(Qt::blue, 1.0);
    m_appCurve->attach(this);

    for (int i = 0; i <= HISTORY_SECONDS; ++i) {
        m_time.append(HISTORY_SECONDS - i);
        m_total.append(0.0);
        m_app.append(0.0);
    }

    readProcStat(&m_lastTotal, &m_lastIdle);

    m_timer = new QTimer(this);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimerUpdated()));
    m_timer->start(1000);
}

int QCpuUsageWidget::readCoreCount()
{
    QFile f(QLatin1String("/proc/cpuinfo"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return 1;

    int n = 0;
    QTextStream in(&f);
    while (!in.atEnd()) {
        if (in.readLine().startsWith(QLatin1String("processor")))
            ++n;
    }
    return n > 0 ? n : 1;
}

bool QCpuUsageWidget::readProcStat(qulonglong *total, qulonglong *idle)
{
    QFile f(QLatin1String("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&f);
    const QString line = in.readLine();          /* "cpu  u n s i ..." */
    if (!line.startsWith(QLatin1String("cpu")))
        return false;

    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 5)
        return false;

    qulonglong sum = 0;
    for (int i = 1; i < parts.size(); ++i)
        sum += parts.at(i).toULongLong();

    *total = sum;
    *idle = parts.at(4).toULongLong();            /* the idle field */
    return true;
}

void QCpuUsageWidget::onTimerUpdated()
{
    qulonglong total = 0, idle = 0;
    if (!readProcStat(&total, &idle))
        return;

    /* Take the deltas BEFORE updating the stored counters -- both the total
     * and the per-process figures are computed against the same interval. */
    const double dTotal = (total > m_lastTotal) ? double(total - m_lastTotal) : 0.0;
    const double dIdle  = (idle  > m_lastIdle)  ? double(idle  - m_lastIdle)  : 0.0;
    m_lastTotal = total;
    m_lastIdle = idle;

    double usage = 0.0;
    if (dTotal > 0.0)
        usage = 100.0 * (dTotal - dIdle) / dTotal;
    usage = qBound(0.0, usage, 100.0);

    m_total.removeFirst();
    m_total.append(usage);

    /* This process's own share, from utime+stime in /proc/self/stat. */
    double appUsage = 0.0;
    QFile self(QLatin1String("/proc/self/stat"));
    if (self.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList f2 = QString::fromLatin1(self.readAll())
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f2.size() > 14) {
            static qulonglong lastSelf = 0;
            const qulonglong now = f2.at(13).toULongLong() + f2.at(14).toULongLong();
            if (dTotal > 0.0 && now >= lastSelf)
                appUsage = 100.0 * double(now - lastSelf) / dTotal;
            lastSelf = now;
        }
    }
    appUsage = qBound(0.0, appUsage, 100.0);

    m_app.removeFirst();
    m_app.append(appUsage);

    m_totalCurve->setSamples(m_time, m_total);
    m_appCurve->setSamples(m_time, m_app);
    replot();
}

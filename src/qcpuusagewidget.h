#ifndef QCPUUSAGEWIDGET_H
#define QCPUUSAGEWIDGET_H

#include <QVector>

#include <qwt_plot.h>
#include <qwt_plot_curve.h>

class QTimer;

/*
 * QCpuUsageWidget -- the "ЦПУ" panel.
 *
 * Reads /proc/stat ("cpu %Ld %Ld %Ld %Ld" @0x49453b) and counts cores from
 * /proc/cpuinfo ("processor" @0x49455d). Plots the last 60 s of total and
 * per-application usage, x axis "t, c" running 60 on the left down to 0 on
 * the right, y axis 0..100 %.
 *
 * moc slots: onTimerUpdated()
 */
class QCpuUsageWidget : public QwtPlot
{
    Q_OBJECT

public:
    explicit QCpuUsageWidget(QWidget *parent = 0);

    int coreCount() const { return m_coreCount; }

public slots:
    void onTimerUpdated();

private:
    /* Total jiffies and idle jiffies from /proc/stat. */
    static bool readProcStat(qulonglong *total, qulonglong *idle);
    static int  readCoreCount();

    QwtPlotCurve *m_totalCurve;
    QwtPlotCurve *m_appCurve;

    QVector<double> m_time;
    QVector<double> m_total;
    QVector<double> m_app;

    qulonglong m_lastTotal;
    qulonglong m_lastIdle;
    int        m_coreCount;
    QTimer    *m_timer;
};

#endif /* QCPUUSAGEWIDGET_H */

#include "qdrivepiechart.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

#include <sys/statvfs.h>

/* ------------------------------------------------------------------ *
 * QIgDrivePieChartLabel
 * ------------------------------------------------------------------ */

QIgDrivePieChartLabel::QIgDrivePieChartLabel(const QString &name, QObject *parent)
    : QObject(parent), m_name(name)
{
}

void QIgDrivePieChartLabel::setInfo(const float &gigabytes, const unsigned int &percent)
{
    /* "%1GB(%2%)" @0x494603 is the recovered format string. The original's
     * panel renders only the "NNN.NNNGB" part, which is what getShortString()
     * returns and what the legend actually draws. */
    m_info = QString(QLatin1String("%1GB(%2%)"))
                 .arg(gigabytes, 0, 'f', 3)
                 .arg(percent);
    m_short = QString(QLatin1String("%1GB")).arg(gigabytes, 0, 'f', 3);
}

int QIgDrivePieChartLabel::getHeight() const
{
    return QFontMetrics(m_font).height();
}

int QIgDrivePieChartLabel::getWidth() const
{
    const QString text = m_name + QLatin1Char(' ') + m_info;
    return QFontMetrics(m_font).boundingRect(text).width();
}

/* ------------------------------------------------------------------ *
 * QDrivePieChart
 * ------------------------------------------------------------------ */

QDrivePieChart::QDrivePieChart(QWidget *parent)
    : QWidget(parent),
      m_usedGb(0.0),
      m_freeGb(0.0),
      m_timer(0)
{
    setMinimumHeight(90);

    m_usageLabel = new QIgDrivePieChartLabel(QLatin1String("usage"), this);
    m_freeLabel  = new QIgDrivePieChartLabel(QLatin1String("free"), this);

    connect(this, SIGNAL(driveSizesChanged()), this, SLOT(onDriveSizesChanged()));

    m_timer = new QTimer(this);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimerUpdated()));
    m_timer->start(10000);
}

void QDrivePieChart::setPath(const QString &path)
{
    m_path = path;
    onTimerUpdated();
}

void QDrivePieChart::onTimerUpdated()
{
    const QByteArray path = m_path.isEmpty()
                                ? QByteArray("/")
                                : m_path.toLocal8Bit();

    struct statvfs st;
    if (statvfs(path.constData(), &st) != 0)
        return;

    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double total = double(st.f_blocks) * double(st.f_frsize) / gb;
    const double freeGb = double(st.f_bavail) * double(st.f_frsize) / gb;
    const double usedGb = total - freeGb;

    if (qFuzzyCompare(usedGb + 1.0, m_usedGb + 1.0) &&
        qFuzzyCompare(freeGb + 1.0, m_freeGb + 1.0))
        return;

    m_usedGb = usedGb;
    m_freeGb = freeGb;
    emit driveSizesChanged();
}

void QDrivePieChart::onDriveSizesChanged()
{
    const double total = m_usedGb + m_freeGb;
    const unsigned int usedPct = total > 0.0
                                     ? (unsigned int)(100.0 * m_usedGb / total + 0.5)
                                     : 0;
    m_usageLabel->setInfo((float)m_usedGb, usedPct);
    m_freeLabel->setInfo((float)m_freeGb, 100 - usedPct);
    update();
}

void QDrivePieChart::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont small = p.font();
    small.setPointSizeF(qMax(6.5, small.pointSizeF() - 1.5));
    p.setFont(small);
    p.fillRect(rect(), Qt::black);

    const double total = m_usedGb + m_freeGb;
    if (total <= 0.0)
        return;

    /* Pie on the left, legend on the right, as in the original. */
    const int side = qMin(height() - 8, width() / 3);
    const QRect pie(6, (height() - side) / 2, side, side);

    const int usedSpan = int(360.0 * 16.0 * m_usedGb / total);

    p.setPen(Qt::NoPen);
    p.setBrush(Qt::red);
    p.drawPie(pie, 90 * 16, -usedSpan);
    p.setBrush(Qt::green);
    p.drawPie(pie, 90 * 16 - usedSpan, -(360 * 16 - usedSpan));

    const int x = pie.right() + 12;
    const int lineH = fontMetrics().height() + 4;
    int y = (height() - 2 * lineH) / 2;

    p.setPen(Qt::NoPen);
    p.setBrush(Qt::red);
    p.drawRect(x, y + 3, 10, 10);
    p.setPen(Qt::white);
    p.drawText(x + 16, y + lineH - 4,
               m_usageLabel->getName() + QLatin1Char(' ') + m_usageLabel->getShortString());

    y += lineH;
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::green);
    p.drawRect(x, y + 3, 10, 10);
    p.setPen(Qt::white);
    p.drawText(x + 16, y + lineH - 4,
               m_freeLabel->getName() + QLatin1Char(' ') + m_freeLabel->getShortString());
}

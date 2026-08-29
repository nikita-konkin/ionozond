#include "common.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QTextStream>

#include <cmath>

/* ------------------------------------------------------------------ *
 * Combo box contents
 * ------------------------------------------------------------------ */

const QStringList &FFT_COUNT_LIST()
{
    static const QStringList list = QStringList()
        << QLatin1String("512")   << QLatin1String("1024")
        << QLatin1String("2048")  << QLatin1String("4096")
        << QLatin1String("8192")  << QLatin1String("16384")
        << QLatin1String("32768") << QLatin1String("65536");
    return list;
}

const QStringList &SAMPLE_RATE_LIST()
{
    static const QStringList list = QStringList()
        << QLatin1String("3125") << QLatin1String("6250")
        << QLatin1String("12500") << QLatin1String("25000");
    return list;
}

const QStringList &IG_VERTICAL_SCALE_LIST()
{
    static const QStringList list = QStringList()
        << QLatin1String("h, km") << QLatin1String("t, ms");
    return list;
}

/* ------------------------------------------------------------------ *
 * QBaseSoundParams helpers
 * ------------------------------------------------------------------ */

double QBaseSoundParams::sampleRate_Mhz() const
{
    const QStringList &list = SAMPLE_RATE_LIST();
    if (sampleRateIndex < 0 || sampleRateIndex >= list.size())
        return 0.0;
    return list.at(sampleRateIndex).toDouble() / 1000.0;   // kHz -> MHz
}

int QBaseSoundParams::fftCount() const
{
    const QStringList &list = FFT_COUNT_LIST();
    if (fftCountIndex < 0 || fftCountIndex >= list.size())
        return 0;
    return list.at(fftCountIndex).toInt();
}

/* ------------------------------------------------------------------ *
 * Settings locations
 * ------------------------------------------------------------------ */

QString homePath()
{
    return QDir::homePath();
}

QString configIniPath()
{
    return homePath() + QLatin1String("/.config/dsChirp/config.ini");
}

QString scheduleIniPath()
{
    return homePath() + QLatin1String("/.config/dsChirp/schedule.ini");
}

/* ------------------------------------------------------------------ *
 * Geometry, ported from ionogr_clean/distance.py
 * ------------------------------------------------------------------ */

namespace {
const double EARTH_RADIUS_KM = 6378.137;
const double LAY_E_SQR       = 40000.0;   /* also present in .rodata @0x494448 */

inline double radians(double deg) { return deg * M_PI / 180.0; }
}

double earthDistanceKm(double lat1, double lon1, double lat2, double lon2)
{
    const double txLat = radians(lat1);
    const double txLon = radians(lon1);
    const double rxLat = radians(lat2);
    const double rxLon = radians(lon2);

    const double txLatCos = std::cos(txLat);
    const double rxLatCos = std::cos(rxLat);
    const double txLatSin = std::sin(txLat);
    const double rxLatSin = std::sin(rxLat);

    const double dlon    = rxLon - txLon;
    const double dlonCos = std::cos(dlon);
    const double dlonSin = std::sin(dlon);

    const double x = txLatSin * rxLatSin + txLatCos * rxLatCos * dlonCos;
    const double y = std::sqrt(std::pow(rxLatCos * dlonSin, 2.0) +
                               std::pow(txLatCos * rxLatSin - txLatSin * rxLatCos, 2.0));

    return EARTH_RADIUS_KM * std::atan2(y, x);
}

double rayDistanceKm(double earthDistance)
{
    return std::sqrt(LAY_E_SQR + earthDistance * earthDistance);
}

/* ------------------------------------------------------------------ *
 * Logging
 *
 * myMessageHandler @0x44ba00 writes "<dataDir>/logs/<yy-MM-dd>.log" with a
 * severity prefix and a "Class::method" context extracted by regex.
 * ------------------------------------------------------------------ */

namespace {
QString g_logDir;
QMutex  g_logMutex;
}

void setLogDirectory(const QString &dataDir)
{
    QMutexLocker lock(&g_logMutex);
    g_logDir = dataDir;
    if (!g_logDir.isEmpty())
        QDir().mkpath(g_logDir + QLatin1String("/logs"));
}

void myMessageHandler(QtMsgType type, const QMessageLogContext &context,
                      const QString &msg)
{
    QMutexLocker lock(&g_logMutex);
    if (g_logDir.isEmpty())
        return;

    const char *prefix = "Info: ";
    switch (type) {
    case QtDebugMsg:    prefix = "Info: ";     break;
    case QtInfoMsg:     prefix = "Info: ";     break;
    case QtWarningMsg:  prefix = "Warning: ";  break;
    case QtCriticalMsg: prefix = "Critical: "; break;
    case QtFatalMsg:    prefix = "Fatal: ";    break;
    }

    const QString path = QString(QLatin1String("%1/logs/%2.log"))
        .arg(g_logDir)
        .arg(QDateTime::currentDateTime().toString(QLatin1String("yy-MM-dd")));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&file);
    out << prefix << msg;
    if (context.function)
        out << " (" << context.function << ")";
    out << "\n";
    file.close();
}

/* ------------------------------------------------------------------ *
 * DigitalClock
 *
 * The control panel's UTC readout: HH:MM:SS with a colon that blinks once a
 * second, and a timeChanged() signal that drives the whole schedule.
 * ------------------------------------------------------------------ */

#include <QTimer>

DigitalClock::DigitalClock(QWidget *parent)
    : QLCDNumber(parent), m_colonVisible(true)
{
    setSegmentStyle(Flat);
    setDigitCount(8);
    setFrameStyle(QFrame::NoFrame);

    QTimer *timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(showTime()));
    timer->start(1000);

    showTime();
}

void DigitalClock::showTime()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    QString text = now.time().toString(QLatin1String("hh:mm:ss"));
    if (!m_colonVisible)
        text.replace(QLatin1Char(':'), QLatin1Char(' '));
    m_colonVisible = !m_colonVisible;

    display(text);
    emit timeChanged(now);
}

/* ------------------------------------------------------------------ *
 * QImageComboBox
 *
 * Draws the selected colour map as a gradient rather than a label. Each item
 * carries its colour-map index in Qt::UserRole.
 * ------------------------------------------------------------------ */

#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QStylePainter>

QImageComboBox::QImageComboBox(QWidget *parent)
    : QComboBox(parent)
{
}

void QImageComboBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));

    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    opt.currentText.clear();          /* the gradient replaces the label */
    painter.drawComplexControl(QStyle::CC_ComboBox, opt);

    QRect field = style()->subControlRect(QStyle::CC_ComboBox, &opt,
                                          QStyle::SC_ComboBoxEditField, this);
    field.adjust(1, 1, -1, -1);
    if (!field.isValid())
        return;

    const int index = currentIndex() >= 0 ? currentIndex() : 0;
    const QVector<QColorLevel> &levels = colorLevelsForIndex(
        itemData(index, Qt::UserRole).isValid()
            ? itemData(index, Qt::UserRole).toInt()
            : index);
    if (levels.isEmpty())
        return;

    /* Stops run from the interval minimum (first) to the maximum (last). */
    QLinearGradient gradient(field.topLeft(), field.topRight());
    const double lo = levels.first().level();
    const double hi = levels.last().level();
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    for (int i = 0; i < levels.size(); ++i) {
        double pos = (levels.at(i).level() - lo) / span;
        pos = qBound(0.0, pos, 1.0);
        gradient.setColorAt(pos, levels.at(i).color());
    }

    painter.fillRect(field, gradient);
}

/* ------------------------------------------------------------------ *
 * QSessionInfoWidget
 *
 * Stylesheets and the "waiting..." text come from the original's .rodata:
 *   "border: 2px solid lightgrey; border-radius: 5px;"  @0x493b88
 *   "color:green;" "color:yellow;" "color:darkred;" "color:white"
 *   "waiting..."                                        @0x493aa3
 * ------------------------------------------------------------------ */

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

QSessionInfoWidget::QSessionInfoWidget(const QString &stationName, const int &index,
                                       QWidget *parent)
    : QWidget(parent),
      m_stationName(stationName),
      m_index(index),
      m_active(false),
      m_check(0),
      m_progress(0),
      m_startLabel(0),
      m_stopLabel(0)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(1);

    m_check = new QCheckBox(m_stationName, this);
    m_check->setChecked(true);
    m_check->setStyleSheet(QLatin1String("color: rgb(255,255,255); font-weight: bold;"));
    connect(m_check, SIGNAL(stateChanged(int)), this, SLOT(setIgVisibleState(int)));
    layout->addWidget(m_check);

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFixedHeight(22);
    m_progress->setStyleSheet(QLatin1String(
        "QProgressBar { border: 2px solid lightgrey; border-radius: 5px; "
        "background-color: rgb(0,0,0); }"
        "QProgressBar::chunk { background-color: rgb(60,130,220); }"));
    layout->addWidget(m_progress);

    QHBoxLayout *times = new QHBoxLayout();
    times->setContentsMargins(0, 0, 0, 0);
    m_startLabel = new QLabel(QLatin1String("waiting..."), this);
    m_startLabel->setStyleSheet(QLatin1String("color:yellow;"));
    m_stopLabel = new QLabel(QString(), this);
    m_stopLabel->setStyleSheet(QLatin1String("color:darkred;"));
    m_stopLabel->setAlignment(Qt::AlignRight);
    times->addWidget(m_startLabel);
    times->addWidget(m_stopLabel);
    layout->addLayout(times);
}

void QSessionInfoWidget::SetSessionTimes(const QDateTime &start, const QDateTime &stop)
{
    m_start = start;
    m_stop = stop;

    m_startLabel->setStyleSheet(QLatin1String("color:green;"));
    m_startLabel->setText(start.toUTC().toString(QLatin1String("hh:mm:ss")));
    m_stopLabel->setText(stop.toUTC().toString(QLatin1String("hh:mm:ss")));
}

void QSessionInfoWidget::SetCurrentDateTime(const QDateTime &dateTime)
{
    if (!m_start.isValid() || !m_stop.isValid())
        return;

    const qint64 span = m_start.secsTo(m_stop);
    if (span <= 0)
        return;

    const qint64 done = m_start.secsTo(dateTime);
    if (done < 0) {
        m_progress->setValue(0);
    } else if (done >= span) {
        m_progress->setValue(1000);
    } else {
        m_progress->setValue(int(1000 * done / span));
    }
}

void QSessionInfoWidget::SetActive(const bool &active)
{
    m_active = active;
    if (!active) {
        m_progress->setValue(0);
        m_progress->setFormat(QLatin1String("stopped"));
        m_progress->setTextVisible(true);
        m_startLabel->setStyleSheet(QLatin1String("color:yellow;"));
        m_startLabel->setText(QLatin1String("waiting..."));
        m_stopLabel->clear();
    }
}

void QSessionInfoWidget::SetSounderStatus(const QString &state, double fraction,
                                          const QString &detail)
{
    /*
     * The state goes on the progress bar, not over the start time. Writing it
     * into m_startLabel replaced the one number an operator most wants while a
     * sounding runs -- when the sweep began -- with text that says the same
     * thing the bar already shows.
     */
    if (fraction >= 0.0)
        m_progress->setValue(int(1000.0 * qBound(0.0, fraction, 1.0)));

    QString text = state;
    QString chunk = QLatin1String("rgb(60,130,220)");

    if (state == QLatin1String("capturing")) {
        text = QString(QLatin1String("recording  %1%"))
                   .arg(int(100.0 * qBound(0.0, fraction, 1.0)));
    } else if (state == QLatin1String("writing")) {
        chunk = QLatin1String("rgb(0,170,200)");
        text = QLatin1String("building products");
    } else if (state == QLatin1String("clean")) {
        chunk = QLatin1String("rgb(60,180,75)");
        text = QLatin1String("captured");
        m_progress->setValue(1000);
    } else if (state == QLatin1String("degraded")) {
        /* A complete sounding that lost samples. Worth showing differently
         * from a failure: the file is there and still holds a trace. */
        chunk = QLatin1String("rgb(230,150,30)");
        text = QLatin1String("captured, lossy");
        m_progress->setValue(1000);
    } else if (state == QLatin1String("failed")) {
        chunk = QLatin1String("rgb(200,50,50)");
        text = QLatin1String("no data");
        m_progress->setValue(0);
    } else if (state == QLatin1String("waiting")) {
        text = QLatin1String("waiting");
        m_progress->setValue(0);
    }

    if (!detail.isEmpty())
        text += QLatin1String("   ") + detail;

    m_progress->setFormat(text);
    m_progress->setTextVisible(true);
    m_progress->setStyleSheet(
        QString(QLatin1String(
            "QProgressBar { border: 2px solid lightgrey; border-radius: 5px; "
            "background-color: rgb(0,0,0); color: rgb(255,255,255); }"
            "QProgressBar::chunk { background-color: %1; }")).arg(chunk));
}

void QSessionInfoWidget::setIgVisibleState(const int &igVisibleState)
{
    emit igVisibleChanged(m_stationName, igVisibleState != Qt::Unchecked);
}

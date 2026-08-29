#ifndef COMMON_H
#define COMMON_H

#include "qigcolormap.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QWidget>
#include <QLCDNumber>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <qwt_interval.h>
#include <qwt_samples.h>

/*
 * Shared types and constants.
 *
 * The typedefs below were recovered by comparing the moc parameter type names
 * against the demangled C++ signatures: where moc says "QIgWindow" the mangled
 * name says QRectF, so QIgWindow is a typedef, not a class.
 */

typedef QRectF                     QIgWindow;   // moc: QIgWindow
typedef QVector<float>             SnrVector;   // moc: SnrVector
typedef QVector<QwtIntervalSample> PdpVector;   // moc: PdpVector

/* Real structs (the mangled names keep these spellings). */

struct QIgInterval {
    double min;
    double max;
    QIgInterval(double mn = 0.0, double mx = 0.0) : min(mn), max(mx) {}
};

struct QIgArea {
    QIgInterval x;
    QIgInterval y;
};

struct QIgDataArraySize {
    unsigned int specCount;
    unsigned int specPointCount;
    QIgDataArraySize(unsigned int sc = 0, unsigned int spc = 0)
        : specCount(sc), specPointCount(spc) {}
};

/* Transmitter parameters, one per station group in schedule.ini. */
struct QTxParams {
    QString name;
    double  lat;
    double  lon;
    quint32 chirpt;      // s into the repetition period
    quint32 cf;          // kHz as stored in the .ini
    quint32 dur;         // s
    quint32 rate;        // kHz/s as stored in the .ini
    quint32 rep;         // s
    double  snrMax;
    double  pdpMax;
    bool    active;
    bool    rx;

    QTxParams()
        : lat(0), lon(0), chirpt(0), cf(0), dur(0), rate(0), rep(0),
          snrMax(0), pdpMax(0), active(false), rx(false) {}
};

/* The receiving station. */
struct QRxParams {
    QString name;
    double  lat;
    double  lon;
    QRxParams() : lat(0), lon(0) {}
};

/*
 * Sounding parameters common to every station, read from config.ini by
 * frmMain::getBaseSoundParams() @0x44ecb0.
 */
struct QBaseSoundParams {
    QString dataDir;
    QString soundApp;
    int     sampleRateIndex;
    int     fftCountIndex;
    int     colormapIndex;      // colormap_index: variation plots
    int     igColormapIndex;    // ig_colormap_index: the ionogram
    bool    colormapGradient;
    int     igVerticalScaleIndex;
    int     igListCount;
    bool    whiten;
    int     whitenLen;
    int     whitenN;
    bool    snrAutoMax;
    bool    pdpAutoMax;

    /*
     * The speckle filter, which decides how much of a faint trace survives.
     * Hard-coded in the original; settable here because it is the one control
     * that trades sensitivity against noise, and the right value depends on
     * the path and the interference at a given site. Defaults are the
     * original's.
     */
    unsigned int objSizeH;      // obj_size_horizontal: window in spectra
    unsigned int objSizeV;      // obj_size_vertical: window in delay rows
    float        objLevel;      // obj_level: neighbours a point must have

    QBaseSoundParams()
        : sampleRateIndex(0), fftCountIndex(0), colormapIndex(0),
          igColormapIndex(1), colormapGradient(false), igVerticalScaleIndex(0), igListCount(0),
          whiten(false), whitenLen(0), whitenN(0),
          snrAutoMax(false), pdpAutoMax(false),
          objSizeH(9), objSizeV(3), objLevel(11.0f) {}

    /* Sampling rate in MHz, from SAMPLE_RATE_LIST (which is in kHz). */
    double sampleRate_Mhz() const;
    /* FFT length, from FFT_COUNT_LIST. */
    int    fftCount() const;
};

/* "Суточный ход" parameters: 3 ints and 2 ints respectively. */
struct SnrVarParams {
    int periodHour;
    int timeBandMin;
    int freqBandKHz;
    bool autoMax;
    SnrVarParams() : periodHour(0), timeBandMin(0), freqBandKHz(0), autoMax(false) {}
};

struct PdpVarParams {
    int periodHour;
    int timeBandMin;
    bool autoMax;
    PdpVarParams() : periodHour(0), timeBandMin(0), autoMax(false) {}
};

/* ------------------------------------------------------------------ *
 * Constants recovered from .rodata.
 *
 * The combo lists are stored ASCENDING. The static initialisers construct
 * the strings in descending order and then append them in reverse, which is
 * why fft_count_index=5 selects 16384 and sample_rate_index=3 selects 25000
 * (both confirmed against the running original's Parameters dialog).
 * ------------------------------------------------------------------ */

const QStringList &FFT_COUNT_LIST();          // 512 .. 65536
const QStringList &SAMPLE_RATE_LIST();        // 3125 .. 25000, kHz
const QStringList &IG_VERTICAL_SCALE_LIST();  // "h, km", "t, ms"

/* Virtual height limits, matching ionogr_clean/ig_utils1.py. */
const float VIRT_HEIGHT_MIN                     = -60000.0f;
const float VIRT_HEIGHT_MAX                     =  60000.0f;
const float VIRT_HEIGHT_WINDOW_KM_DEFAULT       =   1500.0f;
const float VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT =   180.0f;

const double LIGHT_SPEED_KM_S       = 300e3;
const double MILLISECONDS_IN_SECOND = 1e3;
const double HZ_IN_MHZ              = 1e6;

/* Axis titles. */
const char *const AXIS_FREQ_TITLE       = "f, MHz";
const char *const TIME_AXIS_TITLE_TEXT  = "t, ms";
const char *const DELAY_AXIS_TITLE_TEXT = "t, ms";

/* Settings locations: the original builds these from $HOME rather than
 * relying on QSettings' own path resolution. */
QString homePath();
QString configIniPath();     // $HOME/.config/dsChirp/config.ini
QString scheduleIniPath();   // $HOME/.config/dsChirp/schedule.ini

/* Great-circle distance between two stations, in km. */
double earthDistanceKm(double lat1, double lon1, double lat2, double lon2);
/* Ray path length for a given ground distance, ported from
 * ionogr_clean/distance.py::ray_distance. */
double rayDistanceKm(double earthDistance);

/* Conversions used by the ionogram axes. */
inline double timeMsFromHeightKm(double heightKm)
{ return MILLISECONDS_IN_SECOND * (heightKm / LIGHT_SPEED_KM_S); }

inline double heightKmFromTimeMs(double timeMs)
{ return LIGHT_SPEED_KM_S * (timeMs / MILLISECONDS_IN_SECOND); }

/*
 * DigitalClock - the green UTC readout in the control panel.
 * moc: signal timeChanged(QDateTime newDateTime), slot showTime().
 * Lives in common.cpp, as it did in the original (there is no
 * digitalclock.cpp among the recovered translation units).
 */
class DigitalClock : public QLCDNumber
{
    Q_OBJECT
public:
    explicit DigitalClock(QWidget *parent = 0);

signals:
    void timeChanged(const QDateTime &newDateTime);

public slots:
    void showTime();

private:
    bool m_colonVisible;
};

/*
 * QImageComboBox @0x47ee80 -- the "Цвета" combo. Instead of text it paints the
 * selected colour map as a horizontal gradient across the field, which is what
 * the original's Parameters dialog shows.
 */
class QImageComboBox : public QComboBox
{
    Q_OBJECT
public:
    explicit QImageComboBox(QWidget *parent = 0);

protected:
    virtual void paintEvent(QPaintEvent *event);
};

/*
 * QSessionInfoWidget @0x4?????  -- one row in the "Сеансы" panel.
 *
 * A checkbox carrying the station name, a progress bar spanning the session
 * window, and the start/stop times underneath (green start, dark red stop),
 * or "waiting..." in yellow before the first session is scheduled.
 *
 * moc: signal igVisibleChanged(stationName, visible), slot setIgVisibleState(int)
 */
class QSessionInfoWidget : public QWidget
{
    Q_OBJECT
public:
    QSessionInfoWidget(const QString &stationName, const int &index, QWidget *parent = 0);

    QString stationName() const { return m_stationName; }

    void SetSessionTimes(const QDateTime &start, const QDateTime &stop);
    void SetCurrentDateTime(const QDateTime &dateTime);
    void SetActive(const bool &active);

    /* Driven by the sounder's own STATUS lines rather than by the clock, so
     * the panel shows what is actually happening instead of what the schedule
     * says ought to be. `fraction` is < 0 when the state carries no progress. */
    void SetSounderStatus(const QString &state, double fraction,
                          const QString &detail = QString());

signals:
    void igVisibleChanged(const QString &stationName, const bool &visible);

public slots:
    void setIgVisibleState(const int &igVisibleState);

private:
    QString    m_stationName;
    int        m_index;
    QDateTime  m_start;
    QDateTime  m_stop;
    bool       m_active;

    class QCheckBox    *m_check;
    class QProgressBar *m_progress;
    class QLabel       *m_startLabel;
    class QLabel       *m_stopLabel;
};

/* Message handler writing <dataDir>/logs/<yy-MM-dd>.log, as the original does. */
void myMessageHandler(QtMsgType type, const QMessageLogContext &context,
                      const QString &msg);
void setLogDirectory(const QString &dataDir);

#endif /* COMMON_H */

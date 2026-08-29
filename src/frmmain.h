#ifndef FRMMAIN_H
#define FRMMAIN_H

#include "common.h"
#include "qigframe.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QMainWindow>
#include <QProcess>
#include <QSettings>

namespace Ui { class frmMain; }

/*
 * Main window.
 *
 * Slot and signal names come from the moc string table of the original:
 *   ReadConsole, ReadConsoleError, ParamsDlgClose, ScheduleDlgClose,
 *   on_btnStartStop_clicked, on_btnParams_clicked, on_btnSchedule_clicked,
 *   setSoundAppFileName(fileName), setConfigFileName(fileName),
 *   setRx(rxName), getRxName, onIgDirNameChanged(dir),
 *   SetCurrentDateTime(dateTime)
 */
class frmMain : public QMainWindow
{
    Q_OBJECT

public:
    explicit frmMain(QWidget *parent = 0);
    ~frmMain();

    QString getSoundAppFileName() const;
    QString getConfigFileName() const;
    QString getRxName() const;
    QString getIgDirName() const;

    QBaseSoundParams getBaseSoundParams() const;
    QRxParams        getRxParams();
    SnrVarParams     getSnrVarParams();
    PdpVarParams     getPdpVarParams();
    QString          getRxNameFromSchedule();

public slots:
    void setSoundAppFileName(const QString &fileName);
    void setConfigFileName(const QString &fileName);
    void setRx(const QString &rxName);
    void onIgDirNameChanged(const QString &dir);
    void SetCurrentDateTime(const QDateTime &dateTime);

    void ReadConsole();
    void ReadConsoleError();
    /* Pick up captures that appear while the console is running. */
    void ScanForNewCaptures();
    void ParamsDlgClose();
    void ScheduleDlgClose();

private slots:
    void on_btnStartStop_clicked();
    void on_btnParams_clicked();
    void on_btnSchedule_clicked();

private:
    void seanseStart();
    void seanseStop();
    bool CreateConfigFile();
    void CreateActiveSchedule();

    /* The sounder's own progress channel. Returns true when the line was a
     * STATUS line and has been consumed, so it never reaches the log pane. */
    bool handleStatusLine(const QString &line);
    class QSessionInfoWidget *sessionWidget(const QString &stationName) const;

    /* True while the sounder says it is recording. Heavy work is held off
     * until it is not -- see ScanForNewCaptures. */
    bool                   m_sounderCapturing;
    QSet<QString>          m_loadedCaptures;
    QHash<QString, qint64> m_growing;      /* path -> size at the last scan */
    class QTimer          *m_scanTimer;

    /* One QIGFrame per active transmitter, added to splWorkPart. */
    void CreateIgAreas();
    /* The session rows only -- rebuilt whenever the schedule changes. */
    void CreateSessionRows();
    /* Rebuild everything the schedule decides, after it has been edited. */
    void RebuildStations();
    /* Session rows, CPU plot and disk pie in the right-hand panel. */
    void CreateControlPanel();

    QList<QSessionInfoWidget *> m_sessionWidgets;
    QList<QTxParams>            m_sessionParams;
    class QCpuUsageWidget      *m_cpuWidget;
    class QDrivePieChart       *m_driveWidget;
    /* Load the most recent capture on disk for each station, so the window
     * has something to show when opened on an archive. */
    void LoadLatestCaptures();

    QList<QIGFrame *> m_igFrames;

    /* Append a line to the console pane in the given colour, the way
     * seanseStart()/seanseStop() do for START and STOP. */
    void console(const QString &text, const QColor &colour);

    Ui::frmMain *ui;

    QSettings *m_settings;        // config.ini
    QSettings *m_scheduleSettings; // schedule.ini
    QProcess  *m_soundProcess;

    QString m_soundAppFileName;
    QString m_configFileName;
    QString m_rxName;
    bool    m_running;
};

#endif /* FRMMAIN_H */

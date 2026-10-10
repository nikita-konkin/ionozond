#include "frmmain.h"
#include "ui_frmmain.h"

#include "configwriter.h"
#include "parametersdialog.h"
#include "scheduledialog.h"
#include "qcpuusagewidget.h"
#include "qdrivepiechart.h"
#include "schedule.h"
#include "schedulecheck.h"
#include "stationstack.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QSet>
#include <QMessageBox>
#include <QSizePolicy>
#include <QTextCursor>
#include <QTextEdit>

/* A schedule number the console only needs whole, which the sounder may be
 * given to a fraction.
 *
 * Sodankyla sweeps at 500.0084 kHz/s from :54.0016 of each minute, and both
 * fractions matter to the sounder: rounding them costs about 1.5 ms, which it
 * reports as 440 km of extra range. The table and chirp_config.py carry the
 * text through untouched, so the sounder gets them. But toUInt() of
 * "121.0015" is 0 -- not 121 -- and a rate of 0 collapses the ionogram's
 * frequency axis. The session panel and the axes are fine with the whole
 * part.
 */
static quint32 wholeSetting(const QVariant &value)
{
    const double v = value.toDouble();
    return v > 0.0 ? (quint32)(v + 0.5) : 0u;
}

frmMain::frmMain(QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::frmMain),
      m_settings(0),
      m_scheduleSettings(0),
      m_soundProcess(0),
      m_cpuWidget(0),
      m_driveWidget(0),
      m_running(false),
      m_sounderCapturing(false),
      m_scanTimer(0),
      m_liveTimer(0),
      m_stations(0)
{
    ui->setupUi(this);

    /*
     * The station panels scroll, and each can be stretched by the grip under
     * it (StationStack). The form's splitter stood here: its minimum was the
     * sum of the panels', so a third station pushed the window past the
     * bottom of the screen and cut off the last panel and the log pane, and
     * no panel could be made taller without making a neighbour shorter. Two
     * stations still fill the area as before; the scroll bar appears only
     * when they do not fit.
     */
    {
        QScrollArea *scroll = new QScrollArea(ui->fraWorkPart);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        delete ui->verticalLayout->replaceWidget(ui->splWorkPart, scroll);
        delete ui->splWorkPart;
        ui->splWorkPart = 0;
        m_stations = new StationStack();
        scroll->setWidget(m_stations);
        m_stations->followViewport(scroll->viewport());
        connect(m_stations, SIGNAL(heightChanged(QString,int)),
                this, SLOT(onStationHeightChanged(QString,int)));
    }

    m_settings = new QSettings(configIniPath(), QSettings::IniFormat, this);
    m_scheduleSettings = new QSettings(scheduleIniPath(), QSettings::IniFormat, this);

    /* DSCHIRP_FIX_UNLOADED_PATHS: both of these arrive only through signals
     * from ParametersDialog, so on a fresh start they were empty until the
     * operator happened to open the parameters dialog and close it. An empty
     * config path makes CreateConfigFile() fail with "Error writing
     * configuration file", and an empty sound_app means seanseStart() silently
     * launches nothing at all -- pressing START appears to work and no sounding
     * ever runs. Load them from the settings they came from. */
    m_soundAppFileName = m_settings->value(QLatin1String("sound_app")).toString();
    m_configFileName   = m_settings->value(QLatin1String("config_file")).toString();

    m_soundProcess = new QProcess(this);
    connect(m_soundProcess, SIGNAL(readyReadStandardOutput()), this, SLOT(ReadConsole()));
    connect(m_soundProcess, SIGNAL(readyReadStandardError()), this, SLOT(ReadConsoleError()));

    /* The clock drives everything time-dependent. */
    connect(ui->lcdClock, SIGNAL(timeChanged(QDateTime)),
            this, SLOT(SetCurrentDateTime(QDateTime)));

    const QBaseSoundParams base = getBaseSoundParams();
    setLogDirectory(base.dataDir);

    m_rxName = getRxNameFromSchedule();

    CreateIgAreas();
    CreateControlPanel();
    LoadLatestCaptures();
    CheckSettings(false);

    /* Poll rather than watch the filesystem: a directory listing every few
     * seconds costs nothing next to a 250 s sounding, and QFileSystemWatcher
     * would have to be re-armed on every new day directory. */
    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(5000);
    connect(m_scanTimer, SIGNAL(timeout()), this, SLOT(ScanForNewCaptures()));
    m_scanTimer->start();

    /* A sounding in progress grows by a column every 0.41 s; drawing every
     * two seconds is smooth enough and costs a handful of FFTs a tick. */
    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(2000);
    connect(m_liveTimer, SIGNAL(timeout()), this, SLOT(UpdateLiveCaptures()));
    m_liveTimer->start();
}

/* One session row per active transmitter, under "Сеансы". Separate from the
 * CPU and disk panels below, which are created once and outlive any schedule
 * change. */
void frmMain::CreateSessionRows()
{
    QVBoxLayout *sessions = qobject_cast<QVBoxLayout *>(ui->fraSessions->layout());
    for (int i = 0; i < m_igFrames.size(); ++i) {
        const QTxParams &tx = m_sessionParams.at(i);

        QSessionInfoWidget *w = new QSessionInfoWidget(tx.name, i, ui->fraSessions);
        connect(w, SIGNAL(igVisibleChanged(QString,bool)),
                m_igFrames.at(i), SLOT(setIgAreaVisible(QString,bool)));
        if (sessions)
            sessions->addWidget(w);
        m_sessionWidgets.append(w);
    }
}

/*
 * The schedule decides which transmitters exist, and it can be edited while
 * the console runs. Everything derived from it -- the ionogram frames, the
 * session rows, the captures loaded for each station -- has to be built again,
 * or the dialog appears to do nothing at all.
 */
void frmMain::RebuildStations(const QString &reason)
{
    for (int i = 0; i < m_igFrames.size(); ++i)
        delete m_igFrames.at(i);
    m_igFrames.clear();

    for (int i = 0; i < m_sessionWidgets.size(); ++i)
        delete m_sessionWidgets.at(i);
    m_sessionWidgets.clear();

    m_sessionParams.clear();
    m_loadedCaptures.clear();
    m_growing.clear();

    m_rxName = getRxNameFromSchedule();

    CreateIgAreas();
    CreateSessionRows();
    LoadLatestCaptures();

    QStringList names;
    for (int i = 0; i < m_sessionParams.size(); ++i)
        names << m_sessionParams.at(i).name;
    console(QString(QLatin1String("%1 reloaded: %2"))
                .arg(reason.isEmpty() ? QLatin1String("schedule") : reason)
                .arg(names.isEmpty() ? QLatin1String("no active transmitters")
                                     : names.join(QLatin1String(", "))),
            Qt::yellow);

    if (m_running) {
        console(QLatin1String("*** the sounder is still running the previous "
                              "schedule -- STOP and START to apply this one"),
                Qt::red);
    } else if (sounderServiceActive()) {
        /*
         * Without this the trap is silent. The console writes chirp_config.py
         * when a dialog is accepted, but a sounder already running has read the
         * old one -- and when that sounder is the service rather than ours,
         * m_running is false and the warning above never fires. The operator
         * sees the panels rebuild, concludes the change took, and the station
         * carries on with the previous settings for as long as nobody restarts
         * it.
         */
        console(QLatin1String("*** ionozond-sounder.service is running and has "
                              "the previous settings"), Qt::red);
        console(QLatin1String("***   sudo systemctl restart ionozond-sounder"),
                Qt::red);
    }
}

/* Is the systemd unit running? False when systemd is absent, which is the
 * right answer for anyone running the sounder by hand. */
bool frmMain::sounderServiceActive()
{
    return QProcess::execute(QLatin1String("systemctl"),
                             QStringList()
                                 << QLatin1String("is-active")
                                 << QLatin1String("--quiet")
                                 << QLatin1String("ionozond-sounder")) == 0;
}

void frmMain::CreateControlPanel()
{
    CreateSessionRows();

    /*
     * Each panel is a heading, a rule, then its plot. Without a stretch factor
     * the spare height is shared out and the heading floats away from the plot
     * it names -- which is what put a gap between "ЦПУ" and its graph. Give the
     * plot the slack and the heading stays put.
     */
    if (QVBoxLayout *cpu = qobject_cast<QVBoxLayout *>(ui->fraCPU->layout())) {
        m_cpuWidget = new QCpuUsageWidget(ui->fraCPU);
        m_cpuWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_cpuWidget->setMinimumHeight(90);
        cpu->addWidget(m_cpuWidget, 1);
        cpu->setAlignment(ui->lblCPU, Qt::AlignTop);
    }

    if (QVBoxLayout *drive = qobject_cast<QVBoxLayout *>(ui->fraDrive->layout())) {
        m_driveWidget = new QDrivePieChart(ui->fraDrive);
        m_driveWidget->setPath(getIgDirName());
        m_driveWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_driveWidget->setMinimumHeight(90);
        drive->addWidget(m_driveWidget, 1);
        drive->setAlignment(ui->lblDrive, Qt::AlignTop);
    }

    /* The session list is as tall as its rows; the plots below take the rest,
     * rather than the sessions frame absorbing it into empty black. */
    ui->fraSessions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    ui->fraCPU->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    ui->fraDrive->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
}

void frmMain::CreateIgAreas()
{
    const QBaseSoundParams base = getBaseSoundParams();
    const QRxParams rx = getRxParams();
    const SnrVarParams snrParams = getSnrVarParams();
    const PdpVarParams pdpParams = getPdpVarParams();

    const QStringList stations = m_scheduleSettings->childGroups();
    for (int i = 0; i < stations.size(); ++i) {
        const QString &station = stations.at(i);

        m_scheduleSettings->beginGroup(station);
        const bool active = m_scheduleSettings->value(QLatin1String("active")).toBool();
        QTxParams tx;
        tx.name   = station;
        tx.lat    = m_scheduleSettings->value(QLatin1String("lat")).toDouble();
        tx.lon    = m_scheduleSettings->value(QLatin1String("lon")).toDouble();
        tx.chirpt = wholeSetting(m_scheduleSettings->value(QLatin1String("chirpt")));
        tx.cf     = wholeSetting(m_scheduleSettings->value(QLatin1String("cf")));
        tx.dur    = wholeSetting(m_scheduleSettings->value(QLatin1String("dur")));
        tx.rate   = wholeSetting(m_scheduleSettings->value(QLatin1String("rate")));
        tx.rep    = wholeSetting(m_scheduleSettings->value(QLatin1String("rep")));
        tx.active = active;
        tx.rx     = m_scheduleSettings->value(QLatin1String("rx")).toBool();
        tx.fftCount    = m_scheduleSettings->value(QLatin1String("fft_count")).toUInt();
        tx.ionoPad     = m_scheduleSettings->value(QLatin1String("iono_pad")).toUInt();
        tx.ionoOverlap = m_scheduleSettings->value(QLatin1String("iono_overlap")).toUInt();
        m_scheduleSettings->endGroup();

        if (!active)
            continue;

        /* The station's own FFT length, so a capture with no sidecar yet is
         * drawn on the grid the sounder will build its sidecar on. */
        QIGFrame *frame = new QIGFrame(base.forStation(tx), tx, rx,
                                       snrParams, pdpParams, this);
        m_igFrames.append(frame);
        m_sessionParams.append(tx);
        m_stations->addStation(frame, station,
                               m_settings->value(QLatin1String("StationHeight/") + station,
                                                 0).toInt());
    }
}

/*
 * Every capture in one day directory, as .lfs paths, whether or not the .lfs
 * still exists.
 *
 * Pruning deletes the 80 MB capture and keeps the 70 kB sidecar, so a glob for
 * *.lfs alone stops finding archived soundings -- silently, since a file that
 * is not enumerated raises nothing. QRxIonogram::load() is already happy to
 * work from the sidecar and never opens the capture when one is there, so all
 * that is needed is to enumerate both and hand it the .lfs name it expects.
 */
QStringList frmMain::CapturesIn(const QDir &day, const QString &station)
{
    /* The glob alone is not enough: "NIC4_*" also matches NIC4_280's
     * captures, which put one station's soundings in another's panels. */
    const QRegularExpression own(
        QLatin1Char('^') + QRegularExpression::escape(station)
        + QLatin1String("_\\d{8}_\\d{6}$"));

    QSet<QString> stems;
    const char *const patterns[] = { "_*.lfs", "_*.lfp" };
    for (int p = 0; p < 2; ++p) {
        const QStringList found = day.entryList(
            QStringList() << (station + QLatin1String(patterns[p])),
            QDir::Files, QDir::Name);
        for (int i = 0; i < found.size(); ++i) {
            const QString stem = QFileInfo(found.at(i)).completeBaseName();
            if (own.match(stem).hasMatch())
                stems.insert(stem);
        }
    }

    QStringList out;
    for (QSet<QString>::const_iterator it = stems.constBegin();
         it != stems.constEnd(); ++it)
        out << day.filePath(*it + QLatin1String(".lfs"));
    out.sort();                         /* by name, which is by time */
    return out;
}

/* Has this capture finished being written?
 *
 * A sounding takes 250 s and half of one renders as noise, so the size has to
 * stop changing before it is touched. With the .lfs pruned there is nothing to
 * watch -- but there is also nothing to wait for: Python writes the sidecar
 * with os.replace, so it appears whole or not at all. */
bool frmMain::CaptureSettled(const QString &lfsPath, QHash<QString, qint64> &growing)
{
    const QFileInfo raw(lfsPath);
    if (!raw.exists()) {
        growing.remove(lfsPath);
        return true;
    }
    const qint64 size = raw.size();
    if (growing.value(lfsPath, -1) != size) {
        growing.insert(lfsPath, size);
        return false;
    }
    growing.remove(lfsPath);
    return true;
}

/*
 * The sounder writes the .lfs as it records, then the .lfp sidecar a few
 * seconds after the last sample. Until the sidecar is there the capture is
 * still the sounder's: loading it then meant computing it here as well,
 * beside the sounder doing the same, and writing a sidecar of our own that
 * the sounder's then replaced -- or, opened in the middle of a sounding,
 * a sidecar of half a capture that nothing replaced in the console's eyes.
 *
 * The age limit is for a sounder that writes no sidecars at all: after that
 * long the console takes the capture over, as it always did.
 */
bool frmMain::AwaitingSounder(const QString &lfsPath)
{
    if (QFile::exists(lfpPathFor(lfsPath)))
        return false;
    const QFileInfo raw(lfsPath);
    if (!raw.exists())
        return false;
    return raw.lastModified().secsTo(QDateTime::currentDateTime()) < 90;
}

void frmMain::LoadLatestCaptures()
{
    const QString dataDir = getIgDirName();
    if (dataDir.isEmpty())
        return;

    /* Captures live in <dataDir>/<yyyy.MM.dd>/<station>_<date>_<time>.lfs */
    QDir root(dataDir);
    QStringList dayDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    /*
     * How far back to load is set by what the variation panels are asked to
     * show, not by a fixed count.
     *
     * This was 24 captures. At the standard 300 s repetition that is exactly
     * two hours, so "период 24 ч" drew two hours of data and the setting
     * looked broken -- the panels trim to the period correctly, they were
     * simply never given more than two hours to trim.
     *
     * Both panels are fed from the same pass, so take whichever period is
     * longer. The ceiling is there because a short repetition and a long
     * period multiply: 24 h at 60 s would be 1440 files, and on an archive
     * whose sidecars have not been built yet each one costs 80 MB and 610
     * FFTs. Loading stays sidecar-first and cheap in the normal case.
     */
    const SnrVarParams snrParams = getSnrVarParams();
    const PdpVarParams pdpParams = getPdpVarParams();
    const int periodHour = qMax(qMax(snrParams.periodHour, pdpParams.periodHour), 1);
    const int CAPTURE_CEILING = 600;

    for (int f = 0; f < m_igFrames.size(); ++f) {
        QIGFrame *frame = m_igFrames.at(f);

        const quint32 rep = (f < m_sessionParams.size()) ? m_sessionParams.at(f).rep : 0;
        int maxCaptures = (rep > 0) ? (periodHour * 3600) / int(rep) : 24;
        maxCaptures = qBound(1, maxCaptures, CAPTURE_CEILING);

        QStringList found;
        for (int d = 0; d < dayDirs.size(); ++d) {
            QDir day(root.filePath(dayDirs.at(d)));
            found << CapturesIn(day, frame->stationName());
        }
        /* Not the one being recorded: the live view draws that, and the
         * scan loads it once the sounder has finished with it. */
        for (int i = found.size() - 1; i >= 0; --i)
            if (AwaitingSounder(found.at(i)))
                found.removeAt(i);

        /* Everything found is remembered as seen, including what the cap skips
         * -- otherwise the periodic scan below would treat the rest of the
         * archive as new and load it a file at a time. */
        for (int i = 0; i < found.size(); ++i)
            m_loadedCaptures.insert(found.at(i));

        const int first = qMax(0, found.size() - maxCaptures);
        const int count = found.size() - first;
        if (count > 0) {
            console(QString(QLatin1String("loading %1 capture(s) for %2 "
                                          "-- %3 h at %4 s"))
                        .arg(count).arg(frame->stationName())
                        .arg(periodHour).arg(rep), Qt::gray);
        }
        for (int i = first; i < found.size(); ++i) {
            /* One line per file floods the console at 288 of them, but silence
             * is worse on a first run with no sidecars, where each file is a
             * real wait. Every tenth, plus the last. */
            const int n = i - first + 1;
            if (n % 10 == 0 || i == found.size() - 1) {
                console(QString(QLatin1String("  %1/%2  %3"))
                            .arg(n).arg(count)
                            .arg(QFileInfo(found.at(i)).fileName()), Qt::gray);
            }
            QApplication::processEvents();
            /* Only the last two end up on screen; everything before them is
             * loaded for the variation panels alone. */
            frame->addIg(found.at(i), i >= found.size() - 2);
        }
    }
}

/*
 * The archive grows while the console is open: a sounding lands every
 * repetition period. Without this the panels show whatever existed at startup
 * and never change, which is exactly what an operator watching a live station
 * does not want.
 */
void frmMain::ScanForNewCaptures()
{
    const QString dataDir = getIgDirName();
    if (dataDir.isEmpty())
        return;

    /*
     * Not while a sounding is being recorded. Loading a capture redraws three
     * Qwt panels, and on a four-core host that competes with a dechirp already
     * consuming half the clock at 25 MS/s -- a burst of dropped packets
     * followed the first automatic load. The gap between soundings is around
     * fifty seconds, which is ample, so waiting costs only that.
     */
    if (m_sounderCapturing)
        return;

    QDir root(dataDir);
    const QStringList dayDirs =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (int f = 0; f < m_igFrames.size(); ++f) {
        QIGFrame *frame = m_igFrames.at(f);

        for (int d = 0; d < dayDirs.size(); ++d) {
            QDir day(root.filePath(dayDirs.at(d)));
            const QStringList caps = CapturesIn(day, frame->stationName());

            for (int c = 0; c < caps.size(); ++c) {
                const QString path = caps.at(c);
                if (m_loadedCaptures.contains(path))
                    continue;
                if (AwaitingSounder(path))
                    continue;
                if (!CaptureSettled(path, m_growing))
                    continue;

                m_loadedCaptures.insert(path);
                console(QString(QLatin1String("new capture  %1"))
                            .arg(QFileInfo(path).fileName()), Qt::cyan);
                QApplication::processEvents();
                frame->addIg(path);
            }
        }
    }
}

/*
 * The sounding being recorded, drawn as it arrives -- what the original's
 * current panel did, polling the capture on a timer.
 *
 * Found on disk rather than through the sounder's STATUS lines, which only
 * reach the console when it launched the sounder itself; the systemd service
 * writes the same files. The newest capture of a station that is still the
 * sounder's is the one being recorded, or the one just recorded and waiting
 * for its sidecar, which then replaces the live picture through addIg.
 *
 * Not held off while capturing, unlike the scan: a tick reads the couple of
 * seconds just written, out of the page cache, and runs a few FFTs on them.
 * Live drawing can be switched off in the parameters dialog.
 */
void frmMain::UpdateLiveCaptures()
{
    const bool enabled = m_settings->value(QLatin1String("live_ionogram"), true).toBool();
    const QString dataDir = getIgDirName();

    /* The day directory is named for the sounding's start, so near midnight
     * the one being recorded can be in yesterday's. */
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList days;
    days << now.addSecs(-600).toString(QLatin1String("yyyy.MM.dd"));
    if (now.toString(QLatin1String("yyyy.MM.dd")) != days.first())
        days << now.toString(QLatin1String("yyyy.MM.dd"));

    QDir root(dataDir);
    for (int f = 0; f < m_igFrames.size(); ++f) {
        QIGFrame *frame = m_igFrames.at(f);

        QString newest;
        if (enabled && !dataDir.isEmpty()) {
            for (int d = 0; d < days.size(); ++d) {
                const QDir day(root.filePath(days.at(d)));
                if (!day.exists())
                    continue;
                const QStringList caps = CapturesIn(day, frame->stationName());
                if (!caps.isEmpty())
                    newest = caps.last();
            }
        }

        if (!newest.isEmpty() && !m_loadedCaptures.contains(newest)
                && AwaitingSounder(newest)) {
            frame->followLive(newest);
            continue;
        }

        /* Nothing to follow. A live capture whose sidecar has arrived is
         * replaced by the scan; one that has vanished without a sidecar was
         * stopped early and renamed .partial, and is dropped here. */
        const LiveIonogram *live = frame->live();
        if (!live)
            continue;
        const QString path = live->path();
        const bool finished = m_loadedCaptures.contains(path)
                              || QFile::exists(lfpPathFor(path));
        if (!enabled || (!finished && !QFile::exists(path)))
            frame->dropLive();
    }
}

void frmMain::closeEvent(QCloseEvent *event)
{
    /*
     * "QProcess: Destroyed while process is still running" was the symptom:
     * the QProcess is parented to this window, so Qt destroyed it during
     * teardown without stopping the child. The shell died and left python
     * holding the radio -- an invisible sounder still streaming at 100 MB/s
     * and still writing .lfs files. Start the console again and the two
     * compete for the same N210, which is exactly the shape of an overflow
     * problem that appears from nowhere.
     *
     * A clean stop lets the sweep in progress finish, which is up to 250 s --
     * far too long to block a window close on -- so this asks instead of
     * choosing for the operator. Whatever they answer, nothing is orphaned.
     */
    if (m_soundProcess && m_soundProcess->state() != QProcess::NotRunning) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Идёт зондирование"),
            tr("Сеанс зондирования ещё не закончен.\n\n"
               "Прервать его и выйти?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        console(QLatin1String("stopping the sounder before exit..."), Qt::gray);
        m_soundProcess->terminate();
        if (!m_soundProcess->waitForFinished(15000)) {
            console(QLatin1String("it did not stop; killing it"), Qt::red);
            m_soundProcess->kill();
            m_soundProcess->waitForFinished(5000);
        }
    }
    QMainWindow::closeEvent(event);
}

frmMain::~frmMain()
{
    /* Belt and braces: closeEvent covers the window being closed, this covers
     * every other way the object can go. Nothing may outlive the console. */
    if (m_soundProcess && m_soundProcess->state() != QProcess::NotRunning) {
        m_soundProcess->kill();
        m_soundProcess->waitForFinished(5000);
    }
    delete ui;
}

/* ------------------------------------------------------------------ *
 * Settings accessors
 * ------------------------------------------------------------------ */

QString frmMain::getSoundAppFileName() const { return m_soundAppFileName; }
QString frmMain::getConfigFileName() const   { return m_configFileName; }
QString frmMain::getRxName() const           { return m_rxName; }

QString frmMain::getIgDirName() const
{
    return m_settings->value(QLatin1String("data_dir")).toString();
}

void frmMain::setSoundAppFileName(const QString &fileName) { m_soundAppFileName = fileName; }
void frmMain::setConfigFileName(const QString &fileName)   { m_configFileName = fileName; }
void frmMain::setRx(const QString &rxName)                 { m_rxName = rxName; }
void frmMain::onIgDirNameChanged(const QString &dir)       { setLogDirectory(dir); }

QBaseSoundParams frmMain::getBaseSoundParams() const
{
    QBaseSoundParams p;
    p.dataDir              = m_settings->value(QLatin1String("data_dir")).toString();
    p.soundApp             = m_settings->value(QLatin1String("sound_app")).toString();
    p.objSizeH             = m_settings->value(QLatin1String("obj_size_horizontal"),
                                               9).toUInt();
    p.objSizeV             = m_settings->value(QLatin1String("obj_size_vertical"),
                                               3).toUInt();
    p.objLevel             = m_settings->value(QLatin1String("obj_level"),
                                               11.0).toFloat();
    p.sampleRateIndex      = m_settings->value(QLatin1String("sample_rate_index")).toInt();
    p.fftCountIndex        = m_settings->value(QLatin1String("fft_count_index")).toInt();
    p.colormapIndex        = m_settings->value(QLatin1String("colormap_index")).toInt();
    p.igColormapIndex      = m_settings->value(QLatin1String("ig_colormap_index")).toInt();

    /*
     * DSCHIRP_FIX_GRADIENT_TYPO
     *
     * The original reads "colormap_gradinet" here (typo), while
     * ParametersDialog reads and writes "colormap_gradient". The result is
     * that the user's gradient choice never reaches the ionogram colour map.
     * Fixed at the user's request; the original behaviour was:
     *
     *     m_settings->value("colormap_gradinet").toBool();
     */
    p.colormapGradient     = m_settings->value(QLatin1String("colormap_gradient")).toBool();

    p.igVerticalScaleIndex = m_settings->value(QLatin1String("ig_vertical_scale_index")).toInt();
    p.igListCount          = m_settings->value(QLatin1String("ig_list_count")).toInt();
    p.whiten               = m_settings->value(QLatin1String("whiten")).toBool();
    p.whitenLen            = m_settings->value(QLatin1String("whiten_len")).toInt();
    p.whitenN              = m_settings->value(QLatin1String("whiten_n")).toInt();
    p.snrAutoMax           = m_settings->value(QLatin1String("Variations/snr_automax")).toBool();
    p.pdpAutoMax           = m_settings->value(QLatin1String("Variations/pdp_automax")).toBool();

    /* The sounder reads these through chirp_config.py, with these defaults
     * (lfp_products.py's IONO_PAD_DEFAULT and IONO_OVERLAP_DEFAULT). */
    /* Quoted as a Python string when set by hand, since chirp_config.py
     * copies the value verbatim. */
    QString ionoMode = m_settings->value(QLatin1String("iono_mode"),
                                         QLatin1String("gated")).toString();
    ionoMode.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
    p.ionoModeSnr          = ionoMode.trimmed() == QLatin1String("snr");
    p.ionoPad              = m_settings->value(QLatin1String("iono_pad"), 4).toInt();
    p.ionoOverlap          = m_settings->value(QLatin1String("iono_overlap"), 2).toInt();
    p.liveIonogram         = m_settings->value(QLatin1String("live_ionogram"), true).toBool();
    return p;
}

QRxParams frmMain::getRxParams()
{
    QRxParams p;
    const QString rx = getRxNameFromSchedule();
    if (rx.isEmpty())
        return p;

    m_scheduleSettings->beginGroup(rx);
    p.name = rx;
    p.lat  = m_scheduleSettings->value(QLatin1String("lat")).toDouble();
    p.lon  = m_scheduleSettings->value(QLatin1String("lon")).toDouble();
    m_scheduleSettings->endGroup();
    return p;
}

QString frmMain::getRxNameFromSchedule()
{
    const QStringList stations = m_scheduleSettings->childGroups();
    for (int i = 0; i < stations.size(); ++i) {
        m_scheduleSettings->beginGroup(stations.at(i));
        const bool isRx = m_scheduleSettings->value(QLatin1String("rx")).toBool();
        m_scheduleSettings->endGroup();
        if (isRx)
            return stations.at(i);
    }
    return QString();
}

SnrVarParams frmMain::getSnrVarParams()
{
    SnrVarParams p;
    m_settings->beginGroup(QLatin1String("Variations"));
    p.periodHour  = m_settings->value(QLatin1String("snr_period_hour")).toInt();
    p.timeBandMin = m_settings->value(QLatin1String("snr_time_band_min")).toInt();
    p.freqBandKHz = m_settings->value(QLatin1String("snr_freq_band_kHz")).toInt();
    p.autoMax     = m_settings->value(QLatin1String("snr_automax")).toBool();
    m_settings->endGroup();
    return p;
}

PdpVarParams frmMain::getPdpVarParams()
{
    PdpVarParams p;
    m_settings->beginGroup(QLatin1String("Variations"));
    p.periodHour  = m_settings->value(QLatin1String("pdp_period_hour")).toInt();
    p.timeBandMin = m_settings->value(QLatin1String("pdp_time_band_min")).toInt();
    p.autoMax     = m_settings->value(QLatin1String("pdp_automax")).toBool();
    m_settings->endGroup();
    return p;
}

/* ------------------------------------------------------------------ *
 * Session control
 * ------------------------------------------------------------------ */

void frmMain::console(const QString &text, const QColor &colour)
{
    /*
     * Stamp every line. An unattended station's log is read after the fact,
     * and "3 overflows" means nothing without knowing which sounding it came
     * from. Continuation lines from the sounder's own advisories are indented
     * instead of stamped, so a multi-line note reads as one entry.
     */
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QLatin1String("hh:mm:ss"));

    QString body = text;
    bool continuation = false;
    if (body.startsWith(QLatin1String("***"))) {
        body = body.mid(3).trimmed();
        continuation = true;
    }

    /* Keep the tail bounded: this runs for days. */
    if (ui->txtConsole->document()->blockCount() > 2000) {
        QTextCursor cur(ui->txtConsole->document());
        cur.movePosition(QTextCursor::Start);
        cur.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 500);
        cur.removeSelectedText();
    }

    ui->txtConsole->setTextColor(colour);
    ui->txtConsole->append(continuation
                               ? QString(QLatin1String("         %1")).arg(body)
                               : QString(QLatin1String("%1  %2")).arg(stamp, body));
    ui->txtConsole->setTextColor(Qt::green);
    ui->txtConsole->moveCursor(QTextCursor::End);
}

void frmMain::on_btnStartStop_clicked()
{
    if (m_running)
        seanseStop();
    else
        seanseStart();
}

void frmMain::seanseStart()
{
    console(QLatin1String("=== START ==="), Qt::green);

    /* An error here is a sounder that will not start, crash on the first
     * capture, or record nothing usable: worth one question. */
    if (CheckSettings(false) > 0
            && QMessageBox::question(
                   this, QString::fromUtf8("Запуск зондирования"),
                   QString::fromUtf8("Проверка расписания и параметров нашла ошибки "
                                     "(они в журнале ниже). Запустить всё равно?"),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes) {
        console(QLatin1String("START cancelled."), Qt::yellow);
        return;
    }

    if (!CreateConfigFile()) {
        console(QLatin1String("Error writing configuration file"), Qt::red);
        return;
    }
    CreateActiveSchedule();

    /* DSCHIRP_ARGS_TO_SOUNDER: the original launched the sounding program with
     * no arguments, because gr-juha's chirp.py imported chirp_config from its
     * own working directory. Passing the two paths the console already knows
     * means the sounder does not have to guess where either lives, and one
     * console can drive a sounder installed anywhere. */
    if (!m_soundAppFileName.isEmpty()) {
        QStringList args;
        if (!m_configFileName.isEmpty())
            args << m_configFileName;
        args << getBaseSoundParams().dataDir;
        m_soundProcess->start(m_soundAppFileName, args);
        console(QString(QLatin1String("launching %1 %2"))
                    .arg(m_soundAppFileName, args.join(QLatin1Char(' '))),
                Qt::gray);
    } else {
        console(QLatin1String("No sounding program configured -- nothing will "
                              "run. Set it in the parameters dialog."), Qt::red);
    }

    for (int i = 0; i < m_sessionWidgets.size(); ++i)
        m_sessionWidgets.at(i)->SetActive(true);

    m_running = true;
    ui->btnStartStop->setIcon(QIcon(QLatin1String(":/img/stop_red.png")));
}

void frmMain::seanseStop()
{
    console(QLatin1String("=== STOP ==="), Qt::red);

    /* DSCHIRP_FIX_HARD_KILL: the original sent SIGKILL, which lands in the
     * middle of a sounding and leaves a truncated capture behind. The sounder
     * handles SIGTERM by finishing the sweep in progress, so ask first and
     * only insist if it will not go. A sounding is 250 s, so the wait is
     * generous; the dialog stays responsive because this returns to the event
     * loop between attempts. */
    if (m_soundProcess->state() != QProcess::NotRunning) {
        console(QLatin1String("asking the sounder to finish the current "
                              "sounding..."), Qt::gray);
        m_soundProcess->terminate();
        if (!m_soundProcess->waitForFinished(300000)) {
            console(QLatin1String("it did not stop; killing it"), Qt::red);
            m_soundProcess->kill();
            m_soundProcess->waitForFinished(5000);
        }
    }

    for (int i = 0; i < m_sessionWidgets.size(); ++i)
        m_sessionWidgets.at(i)->SetActive(false);

    m_running = false;
    ui->btnStartStop->setIcon(QIcon(QLatin1String(":/img/play_green.png")));
}

bool frmMain::CreateConfigFile()
{
    if (m_configFileName.isEmpty())
        return false;
    return writeChirpConfig(m_configFileName, *m_scheduleSettings, *m_settings);
}

void frmMain::CreateActiveSchedule()
{
    /* TODO: build QActiveScheduleItem objects and the session widgets.
     * The timing itself is already implemented and tested in schedule.cpp. */
}

void frmMain::SetCurrentDateTime(const QDateTime &dateTime)
{
    /*
     * The clock tick drives the schedule: each row recomputes the session it
     * is waiting for and advances its progress bar. Timing comes from
     * calcNextSessionTimes(), which is verified against the original.
     */
    for (int i = 0; i < m_sessionWidgets.size(); ++i) {
        const QTxParams &tx = m_sessionParams.at(i);
        if (!m_running)
            continue;

        const SessionTimes t = calcNextSessionTimes(dateTime, tx.rep, tx.chirpt, tx.dur);
        m_sessionWidgets.at(i)->SetSessionTimes(t.start, t.stop);
        m_sessionWidgets.at(i)->SetCurrentDateTime(dateTime);
    }
}

QSessionInfoWidget *frmMain::sessionWidget(const QString &stationName) const
{
    for (int i = 0; i < m_sessionWidgets.size(); ++i)
        if (m_sessionWidgets.at(i)->stationName() == stationName)
            return m_sessionWidgets.at(i);
    return 0;
}

/*
 * The sounder reports itself on stdout. Lines beginning "STATUS " are for the
 * session panel and are not echoed; everything else is log text.
 *
 *   STATUS <station> waiting   <start-epoch> <stop-epoch>
 *   STATUS <station> capturing <fraction 0..1> <overflows>
 *   STATUS <station> writing
 *   STATUS <station> clean|degraded|failed <overflows> <samples>
 */
bool frmMain::handleStatusLine(const QString &line)
{
    if (!line.startsWith(QLatin1String("STATUS ")))
        return false;

    const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (f.size() < 3)
        return true;                       /* malformed, but still not log text */

    QSessionInfoWidget *w = sessionWidget(f.at(1));
    if (!w)
        return true;

    const QString state = f.at(2);
    m_sounderCapturing = (state == QLatin1String("capturing"));

    if (state == QLatin1String("waiting") && f.size() >= 5) {
        w->SetSessionTimes(QDateTime::fromMSecsSinceEpoch(f.at(3).toLongLong() * 1000,
                                                          Qt::UTC),
                           QDateTime::fromMSecsSinceEpoch(f.at(4).toLongLong() * 1000,
                                                          Qt::UTC));
        w->SetSounderStatus(state, -1.0);
    } else if (state == QLatin1String("capturing") && f.size() >= 4) {
        const QString overflows = (f.size() >= 5 && f.at(4).toInt() > 0)
                                      ? QString(QLatin1String("(%1 lost)")).arg(f.at(4))
                                      : QString();
        w->SetSounderStatus(state, f.at(3).toDouble(), overflows);
    } else {
        w->SetSounderStatus(state, -1.0);
    }
    return true;
}

void frmMain::ReadConsole()
{
    const QString text = QString::fromLocal8Bit(m_soundProcess->readAllStandardOutput());
    if (text.isEmpty())
        return;

    /* A read can carry several lines, and status lines have to be picked out
     * of the stream one at a time rather than trimmed as a block. */
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        if (!handleStatusLine(line))
            console(line, Qt::green);
    }
}

void frmMain::ReadConsoleError()
{
    const QString text = QString::fromLocal8Bit(m_soundProcess->readAllStandardError());
    if (!text.isEmpty())
        console(text.trimmed(), Qt::red);
}

/* ------------------------------------------------------------------ *
 * Dialogs
 * ------------------------------------------------------------------ */

void frmMain::on_btnParams_clicked()
{
    ParametersDialog *dlg = new ParametersDialog(m_settings, this);

    connect(dlg, SIGNAL(changeSoundAppFileName(QString)),
            this, SLOT(setSoundAppFileName(QString)));
    connect(dlg, SIGNAL(changeConfigFileName(QString)),
            this, SLOT(setConfigFileName(QString)));
    connect(dlg, SIGNAL(igDirChanged(QString)),
            this, SLOT(onIgDirNameChanged(QString)));
    connect(dlg, SIGNAL(finished(int)), this, SLOT(ParamsDlgClose()));

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void frmMain::on_btnSchedule_clicked()
{
    ScheduleDialog *dlg = new ScheduleDialog(m_scheduleSettings, this);

    connect(dlg, SIGNAL(setRxName(QString)), this, SLOT(setRx(QString)));
    connect(dlg, SIGNAL(finished(int)), this, SLOT(ScheduleDlgClose()));

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

/* Settings reach the sounder only through chirp_config.py, and the sounder
 * reads that file once, at startup. So a change made while it is running is
 * real on disk and invisible in the data, for as long as nobody restarts it.
 *
 * This cost an evening: a chirptime edited in the schedule dialog, OK pressed,
 * and every capture afterwards still landing on the old second -- with nothing
 * anywhere saying why, because every dialog and every file agreed with each
 * other and only the running process disagreed.
 */
void frmMain::ApplyConfigChange(const QString &what)
{
    if (!CreateConfigFile()) {
        console(QLatin1String("Error writing configuration file"), Qt::red);
        return;
    }
    if (m_running) {
        console(QString(QLatin1String(
                    "%1 saved, but the running sounder is still using the "
                    "settings it started with.")).arg(what), Qt::yellow);
        console(QLatin1String(
                    "  Press STOP and then START to apply them."), Qt::yellow);
    }
}

void frmMain::ParamsDlgClose()
{
    /*
     * Was an empty stub, so every parameter change looked like it had done
     * nothing until the next restart. The ionogram widgets take the base
     * parameters by value at construction -- there is no setter -- so the
     * honest way to apply a change is to build them again, which also
     * re-renders the stored captures through the new filter settings.
     */
    RebuildStations(QLatin1String("parameters"));

    /*
     * And write chirp_config.py, which until now only START did.
     *
     * That was defensible while the file only described a sounding: nothing
     * read it between runs. It is not defensible now that it carries settings
     * for tools which run on their own -- the NAS sync on its hourly timer
     * reads nas_dest from here, and a destination typed into the dialog that
     * reaches no file until the next START is a setting that silently does
     * nothing.
     *
     * Safe against a running sounder: it reads the file once at startup and
     * never again, and writeChirpConfig now commits atomically.
     */
    ApplyConfigChange(QLatin1String("Parameters"));
    /* The parameters dialog has no list of its own, unlike the schedule's. */
    CheckSettings(true);
}

/* Remembered per station, outside [General] so it never reaches
 * chirp_config.py. */
void frmMain::onStationHeightChanged(const QString &name, int height)
{
    const QString key = QLatin1String("StationHeight/") + name;
    if (height > 0)
        m_settings->setValue(key, height);
    else
        m_settings->remove(key);
}

int frmMain::CheckSettings(bool popupNew)
{
    const QList<CheckIssue> issues = checkSchedule(readScheduleEntries(*m_scheduleSettings),
                                                   readCheckConfig(*m_settings));
    QStringList lines;
    QList<CheckIssue> fresh;
    for (int i = 0; i < issues.size(); ++i) {
        const CheckIssue &is = issues.at(i);
        const QString line = issueLine(is);
        lines << line;
        if (is.level != CheckIssue::Info && !m_knownIssues.contains(line))
            fresh << is;
        console(line, is.level == CheckIssue::Error   ? QColor(Qt::red)
                    : is.level == CheckIssue::Warning ? QColor(Qt::yellow)
                                                      : QColor(Qt::gray));
    }
    m_knownIssues = lines;

    if (popupNew && !fresh.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, QString::fromUtf8("Проверка настроек"),
                        QString::fromUtf8("После изменения настроек:<br><br>")
                            + issuesHtml(fresh),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::RichText);
        box.exec();
    }
    return countIssues(issues, CheckIssue::Error);
}
void frmMain::ScheduleDlgClose()
{
    RebuildStations();
    /* The schedule did not write chirp_config.py at all before this: only
     * START did, so a station added or retimed here reached the sounder only
     * by luck of when it was next started. */
    ApplyConfigChange(QLatin1String("Schedule"));
    /* The dialog has already shown these and asked about new ones. */
    CheckSettings(false);
}

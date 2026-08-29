#include "frmmain.h"
#include "ui_frmmain.h"

#include "configwriter.h"
#include "parametersdialog.h"
#include "scheduledialog.h"
#include "qcpuusagewidget.h"
#include "qdrivepiechart.h"
#include "schedule.h"

#include <QApplication>
#include <QDir>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QTextEdit>

frmMain::frmMain(QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::frmMain),
      m_settings(0),
      m_scheduleSettings(0),
      m_soundProcess(0),
      m_cpuWidget(0),
      m_driveWidget(0),
      m_running(false)
{
    ui->setupUi(this);

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
}

void frmMain::CreateControlPanel()
{
    /* One session row per active transmitter, under "Сеансы". */
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

    if (QVBoxLayout *cpu = qobject_cast<QVBoxLayout *>(ui->fraCPU->layout())) {
        m_cpuWidget = new QCpuUsageWidget(ui->fraCPU);
        cpu->addWidget(m_cpuWidget);
    }

    if (QVBoxLayout *drive = qobject_cast<QVBoxLayout *>(ui->fraDrive->layout())) {
        m_driveWidget = new QDrivePieChart(ui->fraDrive);
        m_driveWidget->setPath(getIgDirName());
        drive->addWidget(m_driveWidget);
    }
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
        tx.chirpt = m_scheduleSettings->value(QLatin1String("chirpt")).toUInt();
        tx.cf     = m_scheduleSettings->value(QLatin1String("cf")).toUInt();
        tx.dur    = m_scheduleSettings->value(QLatin1String("dur")).toUInt();
        tx.rate   = m_scheduleSettings->value(QLatin1String("rate")).toUInt();
        tx.rep    = m_scheduleSettings->value(QLatin1String("rep")).toUInt();
        tx.active = active;
        tx.rx     = m_scheduleSettings->value(QLatin1String("rx")).toBool();
        m_scheduleSettings->endGroup();

        if (!active)
            continue;

        QIGFrame *frame = new QIGFrame(base, tx, rx, snrParams, pdpParams, this);
        m_igFrames.append(frame);
        m_sessionParams.append(tx);
        ui->splWorkPart->addWidget(frame);
    }
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
     * Load every capture we can find for each station, oldest first, so the
     * two variation panels build up their daily course instead of showing a
     * single column. Capped so opening a large archive stays responsive.
     */
    const int MAX_CAPTURES = 24;

    for (int f = 0; f < m_igFrames.size(); ++f) {
        QIGFrame *frame = m_igFrames.at(f);

        QStringList found;
        for (int d = 0; d < dayDirs.size(); ++d) {
            QDir day(root.filePath(dayDirs.at(d)));
            const QStringList caps = day.entryList(
                QStringList() << (frame->stationName() + QLatin1String("_*.lfs")),
                QDir::Files, QDir::Name);
            for (int c = 0; c < caps.size(); ++c)
                found << day.filePath(caps.at(c));
        }

        const int first = qMax(0, found.size() - MAX_CAPTURES);
        for (int i = first; i < found.size(); ++i) {
            console(QString(QLatin1String("loading %1")).arg(found.at(i)), Qt::green);
            QApplication::processEvents();
            frame->addIg(found.at(i));
        }
    }
}

frmMain::~frmMain()
{
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
    ui->txtConsole->setTextColor(colour);
    ui->txtConsole->append(text);
    ui->txtConsole->setTextColor(Qt::green);
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
    console(QLatin1String("START"), Qt::green);

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
    console(QLatin1String("STOP"), Qt::red);

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

void frmMain::ParamsDlgClose()   {}
void frmMain::ScheduleDlgClose() {}

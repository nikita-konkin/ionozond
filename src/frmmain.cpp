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

    if (!m_soundAppFileName.isEmpty())
        m_soundProcess->start(m_soundAppFileName, QStringList());

    for (int i = 0; i < m_sessionWidgets.size(); ++i)
        m_sessionWidgets.at(i)->SetActive(true);

    m_running = true;
    ui->btnStartStop->setIcon(QIcon(QLatin1String(":/img/stop_red.png")));
}

void frmMain::seanseStop()
{
    console(QLatin1String("STOP"), Qt::red);
    m_soundProcess->kill();

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

void frmMain::ReadConsole()
{
    const QString text = QString::fromLocal8Bit(m_soundProcess->readAllStandardOutput());
    if (!text.isEmpty())
        console(text.trimmed(), Qt::green);
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

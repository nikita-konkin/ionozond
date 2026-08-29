#include "parametersdialog.h"
#include "ui_parametersdialog.h"

#include "common.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

ParametersDialog::ParametersDialog(QSettings *settings, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ParametersDialog),
      m_settings(settings)
{
    ui->setupUi(this);

    /* The filter and NTP fields follow their enabling checkboxes, exactly as
     * the original wires them up ("1setEnabled(bool)" <- "2toggled(bool)"). */
    connect(ui->chbFilter, SIGNAL(toggled(bool)), ui->edtFilterLen, SLOT(setEnabled(bool)));
    connect(ui->chbFilter, SIGNAL(toggled(bool)), ui->edtFilterPointsCount, SLOT(setEnabled(bool)));
    connect(ui->chbFilter, SIGNAL(toggled(bool)), ui->lblFilterLen, SLOT(setEnabled(bool)));
    connect(ui->chbFilter, SIGNAL(toggled(bool)), ui->lblFilterPointsCount, SLOT(setEnabled(bool)));

    connect(ui->chbDirectSignalCutting, SIGNAL(toggled(bool)),
            ui->edtLfsrPolynomeDegree, SLOT(setEnabled(bool)));
    connect(ui->chbDirectSignalCutting, SIGNAL(toggled(bool)),
            ui->edtImpulseLength, SLOT(setEnabled(bool)));

    ReadSettings();
}

ParametersDialog::~ParametersDialog()
{
    delete ui;
}

void ParametersDialog::SetParamsToComboBox(QComboBox *box, const QStringList &items,
                                           const int &index)
{
    box->clear();
    box->addItems(items);
    if (index >= 0 && index < items.size())
        box->setCurrentIndex(index);
}

void ParametersDialog::ReadSettings()
{
    if (!m_settings)
        return;

    ui->edtSoundApp->setText(m_settings->value(QLatin1String("sound_app")).toString());
    ui->edtPyConfig->setText(m_settings->value(QLatin1String("config_file")).toString());
    ui->edtIGDir->setText(m_settings->value(QLatin1String("data_dir")).toString());

    SetParamsToComboBox(ui->cmbSampleRate, SAMPLE_RATE_LIST(),
                        m_settings->value(QLatin1String("sample_rate_index")).toInt());
    SetParamsToComboBox(ui->cmbFftCount, FFT_COUNT_LIST(),
                        m_settings->value(QLatin1String("fft_count_index")).toInt());
    SetParamsToComboBox(ui->cmbIgVerticalScale, IG_VERTICAL_SCALE_LIST(),
                        m_settings->value(QLatin1String("ig_vertical_scale_index")).toInt());

    ui->edtDec->setText(m_settings->value(QLatin1String("dec")).toString());

    const bool whiten = m_settings->value(QLatin1String("whiten")).toBool();
    ui->chbFilter->setChecked(whiten);
    ui->edtFilterLen->setText(m_settings->value(QLatin1String("whiten_len")).toString());
    ui->edtFilterPointsCount->setText(m_settings->value(QLatin1String("whiten_n")).toString());
    ui->edtFilterLen->setEnabled(whiten);
    ui->edtFilterPointsCount->setEnabled(whiten);
    ui->lblFilterLen->setEnabled(whiten);
    ui->lblFilterPointsCount->setEnabled(whiten);

    /* Colour map combo: one entry per recovered map, index carried in UserRole. */
    ui->cmbIgColormap->clear();
    for (int i = 0; i < 10; ++i) {
        ui->cmbIgColormap->addItem(QString::number(i));
        ui->cmbIgColormap->setItemData(i, i, Qt::UserRole);
    }
    ui->cmbIgColormap->setCurrentIndex(
        m_settings->value(QLatin1String("ig_colormap_index")).toInt());

    ui->chbColorGradient->setChecked(
        m_settings->value(QLatin1String("colormap_gradient")).toBool());

    const bool cutting = m_settings->value(QLatin1String("direct_signal_cutting")).toBool();
    ui->chbDirectSignalCutting->setChecked(cutting);
    ui->edtLfsrPolynomeDegree->setText(
        m_settings->value(QLatin1String("lfsr_polynome_degree")).toString());
    /* DSCHIRP_FIX_EMPTY_TB: an absent or empty "tb" is written back verbatim on
     * OK, and the config writer then emits "tb = " -- a Python syntax error, so
     * the generated chirp_config.py cannot be imported by the sounder at all.
     * Real deployments carried tb = 0; default to that rather than faithfully
     * reproducing a file nothing can read. See docs/reverse-engineering.md. */
    QString impulseLength = m_settings->value(QLatin1String("tb")).toString();
    if (impulseLength.isEmpty())
        impulseLength = QLatin1String("0");
    ui->edtImpulseLength->setText(impulseLength);
    ui->edtLfsrPolynomeDegree->setEnabled(cutting);
    ui->edtImpulseLength->setEnabled(cutting);

    ui->cmbIgListCount->clear();
    for (int i = 1; i <= 6; ++i)
        ui->cmbIgListCount->addItem(QString::number(i));
    const int listCount = m_settings->value(QLatin1String("ig_list_count")).toInt();
    if (listCount >= 1 && listCount <= 6)
        ui->cmbIgListCount->setCurrentIndex(listCount - 1);

    ui->edtNtpServerName->setText(QLatin1String("ns1.volgatech.net"));

    m_settings->beginGroup(QLatin1String("Variations"));
    ui->spbSnrVarPeriod->setValue(m_settings->value(QLatin1String("snr_period_hour")).toInt());
    ui->spbSnrVarTimeBand->setValue(m_settings->value(QLatin1String("snr_time_band_min")).toInt());
    ui->spbSnrVarFreqBand->setValue(m_settings->value(QLatin1String("snr_freq_band_kHz")).toInt());
    ui->chbSnrAutoMax->setChecked(m_settings->value(QLatin1String("snr_automax")).toBool());
    ui->spbPdpVarPeriod->setValue(m_settings->value(QLatin1String("pdp_period_hour")).toInt());
    ui->spbPdpVarTimeBand->setValue(m_settings->value(QLatin1String("pdp_time_band_min")).toInt());
    ui->chbPdpAutoMax->setChecked(m_settings->value(QLatin1String("pdp_automax")).toBool());
    m_settings->endGroup();
}

void ParametersDialog::WriteSettings()
{
    if (!m_settings)
        return;

    m_settings->setValue(QLatin1String("sound_app"), ui->edtSoundApp->text());
    m_settings->setValue(QLatin1String("config_file"), ui->edtPyConfig->text());
    m_settings->setValue(QLatin1String("data_dir"), ui->edtIGDir->text());

    m_settings->setValue(QLatin1String("sample_rate_index"), ui->cmbSampleRate->currentIndex());
    m_settings->setValue(QLatin1String("sample_rate"), ui->cmbSampleRate->currentText());
    m_settings->setValue(QLatin1String("fft_count_index"), ui->cmbFftCount->currentIndex());
    m_settings->setValue(QLatin1String("ig_vertical_scale_index"),
                         ui->cmbIgVerticalScale->currentIndex());
    m_settings->setValue(QLatin1String("dec"), ui->edtDec->text());
    m_settings->setValue(QLatin1String("if_rate"), QLatin1String("sample_rate/dec"));

    m_settings->setValue(QLatin1String("whiten"), ui->chbFilter->isChecked());
    m_settings->setValue(QLatin1String("whiten_len"), ui->edtFilterLen->text());
    m_settings->setValue(QLatin1String("whiten_n"), ui->edtFilterPointsCount->text());

    m_settings->setValue(QLatin1String("ig_colormap_index"), ui->cmbIgColormap->currentIndex());
    m_settings->setValue(QLatin1String("colormap_gradient"), ui->chbColorGradient->isChecked());

    m_settings->setValue(QLatin1String("direct_signal_cutting"),
                         ui->chbDirectSignalCutting->isChecked());
    m_settings->setValue(QLatin1String("lfsr_polynome_degree"),
                         ui->edtLfsrPolynomeDegree->text());
    m_settings->setValue(QLatin1String("tb"), ui->edtImpulseLength->text());
    m_settings->setValue(QLatin1String("ig_list_count"),
                         ui->cmbIgListCount->currentText());

    m_settings->beginGroup(QLatin1String("Variations"));
    m_settings->setValue(QLatin1String("snr_period_hour"), ui->spbSnrVarPeriod->value());
    m_settings->setValue(QLatin1String("snr_time_band_min"), ui->spbSnrVarTimeBand->value());
    m_settings->setValue(QLatin1String("snr_freq_band_kHz"), ui->spbSnrVarFreqBand->value());
    m_settings->setValue(QLatin1String("snr_automax"), ui->chbSnrAutoMax->isChecked());
    m_settings->setValue(QLatin1String("pdp_period_hour"), ui->spbPdpVarPeriod->value());
    m_settings->setValue(QLatin1String("pdp_time_band_min"), ui->spbPdpVarTimeBand->value());
    m_settings->setValue(QLatin1String("pdp_automax"), ui->chbPdpAutoMax->isChecked());
    m_settings->endGroup();

    m_settings->sync();
}

void ParametersDialog::on_btbOkCancel_accepted()
{
    /*
     * The original validates that the configured paths exist and refuses to
     * accept otherwise, showing "<file> не существует". Reproduced, because
     * driving the original showed it is what blocks a session from starting.
     */
    const QString soundApp = ui->edtSoundApp->text();
    const QString config   = ui->edtPyConfig->text();
    const QString igDir    = ui->edtIGDir->text();

    struct { QString path; bool isDir; } checks[] = {
        { soundApp, false }, { config, false }, { igDir, true }
    };

    for (int i = 0; i < 3; ++i) {
        const QFileInfo info(checks[i].path);
        const bool ok = checks[i].isDir ? info.isDir() : info.isFile();
        if (!ok) {
            QMessageBox::critical(
                this, QString::fromUtf8("Параметры"),
                QString::fromUtf8("<b>%1</b> не существует").arg(checks[i].path));
            return;
        }
    }

    WriteSettings();

    emit changeSoundAppFileName(soundApp);
    emit changeConfigFileName(config);
    emit igDirChanged(igDir);

    accept();
}

void ParametersDialog::on_btbOkCancel_rejected()
{
    reject();
}

void ParametersDialog::on_btnSoundApp_clicked()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("Программа зондирования"), ui->edtSoundApp->text());
    if (!f.isEmpty())
        ui->edtSoundApp->setText(f);
}

void ParametersDialog::on_btnPyConfig_clicked()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("Конфигурация"), ui->edtPyConfig->text());
    if (!f.isEmpty())
        ui->edtPyConfig->setText(f);
}

void ParametersDialog::on_btnIGDir_clicked()
{
    const QString d = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("Каталог для ионограмм"), ui->edtIGDir->text());
    if (!d.isEmpty())
        ui->edtIGDir->setText(d);
}

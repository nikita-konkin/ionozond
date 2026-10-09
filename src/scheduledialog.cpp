#include "scheduledialog.h"
#include "ui_scheduledialog.h"

#include "common.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>

/* The choices the parameters dialog offers, so a station cannot pick a
 * detail the sidecar builder was never tested with. */
static const QList<int> &padChoices()
{
    static const QList<int> list = QList<int>() << 1 << 2 << 4 << 8;
    return list;
}

static const QList<int> &overlapChoices()
{
    static const QList<int> list = QList<int>() << 1 << 2 << 4;
    return list;
}

ScheduleDialog::ScheduleDialog(QSettings *scheduleSettings, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ScheduleDialog),
      m_settings(scheduleSettings),
      m_checkTimer(0),
      m_generalFft(16384),
      m_generalPad(4),
      m_generalOverlap(2),
      m_ifRateHz(0.0)
{
    ui->setupUi(this);

    /* What "general" currently means, so the table can say it rather than
     * send the operator to the other dialog to find out. Defaults are the
     * parameters dialog's and lfp_products.py's. */
    {
        QSettings config(configIniPath(), QSettings::IniFormat);
        QBaseSoundParams base;
        base.fftCountIndex = config.value(QLatin1String("fft_count_index"), 5).toInt();
        if (base.fftCount() > 0)
            m_generalFft = base.fftCount();
        m_generalPad     = config.value(QLatin1String("iono_pad"), 4).toInt();
        m_generalOverlap = config.value(QLatin1String("iono_overlap"), 2).toInt();
        /* sample_rate is in kHz in the .ini; chirp_config.py gets it as e3. */
        const double sampleHz = config.value(QLatin1String("sample_rate"), 25000).toDouble() * 1e3;
        const double dec      = config.value(QLatin1String("dec"), 625).toDouble();
        m_ifRateHz = dec > 0.0 ? sampleHz / dec : 0.0;
        m_checkConfig = readCheckConfig(config);
    }

    m_checkTimer = new QTimer(this);
    m_checkTimer->setSingleShot(true);
    m_checkTimer->setInterval(300);
    connect(m_checkTimer, SIGNAL(timeout()), this, SLOT(runChecks()));

    QStringList headers;
    headers << QString::fromUtf8("Использовать в активном расписании")
            << QString::fromUtf8("Период повторения, с")
            << QString::fromUtf8("chirptime, c")
            << QString::fromUtf8("Скорость перестройки ЛЧМ сигнала, кГц/с")
            << QString::fromUtf8("Продолжительность сеанса, с")
            << QString::fromUtf8("Частота программного гетеродина USRP, кГц")
            << QString::fromUtf8("Широта (Ю-, С+)")
            << QString::fromUtf8("Долгота (З-, В+)")
            << QString::fromUtf8("Длина БПФ, точек")
            << QString::fromUtf8("Ячейка ионограммы, кГц × км")
            << QString::fromUtf8("Детализация по задержке")
            << QString::fromUtf8("Детализация по частоте")
            << QString::fromUtf8("Удалить из расписания");

    ui->tblSchedule->setRowCount(RowCount);
    ui->tblSchedule->setVerticalHeaderLabels(headers);
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    /* Wide enough for "общ. 16384" and a fractional rate. Past that the
     * table scrolls instead of squeezing every column to an ellipsis, which
     * at six stations it already did to their names. */
    ui->tblSchedule->horizontalHeader()->setMinimumSectionSize(130);

    ui->tblSchedule->verticalHeaderItem(RowFft)->setToolTip(QString::fromUtf8(
        "Своя длина БПФ для станции; «общ.» — как в окне параметров.\n"
        "Длина задаёт форму ячейки ионограммы: при 100 кГц/с 16384 точки\n"
        "дают 41 кГц × 7,3 км, как в оригинале, а при 500 кГц/с ту же форму\n"
        "даёт 4096 (51 кГц × 5,9 км). Каждое уменьшение вчетверо стоит 6 дБ\n"
        "накопления в ячейке. Вступает в силу после остановки и запуска зондирования."));
    ui->tblSchedule->verticalHeaderItem(RowCell)->setToolTip(QString::fromUtf8(
        "Сколько развёртки и сколько задержки занимает одна ячейка\n"
        "при этой скорости и длине БПФ. Только для справки."));
    ui->tblSchedule->verticalHeaderItem(RowIonoPad)->setToolTip(QString::fromUtf8(
        "Как в окне параметров, но для одной станции. Только картинка:\n"
        "LUF, MUF, С/Ш, ПЗМ и архив .h5 считаются без неё."));
    ui->tblSchedule->verticalHeaderItem(RowIonoOverlap)->setToolTip(
        ui->tblSchedule->verticalHeaderItem(RowIonoPad)->toolTip());

    connect(ui->tblSchedule, SIGNAL(cellChanged(int,int)), this, SLOT(onCellChanged(int,int)));
    connect(ui->cmbRxStation, SIGNAL(currentIndexChanged(int)), this, SLOT(scheduleCheck()));

    ReadSchedule();

    runChecks();
    const QList<CheckIssue> issues = checkSchedule(tableEntries(), m_checkConfig);
    for (int i = 0; i < issues.size(); ++i)
        if (issues.at(i).level != CheckIssue::Info)
            m_initialIssues << issueLine(issues.at(i));
}

ScheduleDialog::~ScheduleDialog()
{
    delete ui;
}

QComboBox *ScheduleDialog::detailCombo(const QString &general, const QList<int> &values,
                                       const QString &format, int current)
{
    QComboBox *box = new QComboBox(ui->tblSchedule);
    box->addItem(general, 0);
    for (int i = 0; i < values.size(); ++i)
        box->addItem(format.arg(values.at(i)), values.at(i));
    /* A value from a hand-edited schedule.ini that is not on the list is
     * shown as it is rather than silently becoming "general". */
    int at = box->findData(current);
    if (at < 0) {
        box->addItem(format.arg(current), current);
        at = box->count() - 1;
    }
    box->setCurrentIndex(at);
    connect(box, SIGNAL(currentIndexChanged(int)), this, SLOT(updateCells()));
    connect(box, SIGNAL(currentIndexChanged(int)), this, SLOT(scheduleCheck()));
    return box;
}

void ScheduleDialog::addStationColumn(const QString &name, bool active, const QString &rep,
                                      const QString &chirpt, const QString &rate,
                                      const QString &dur, const QString &cf,
                                      const QString &lat, const QString &lon,
                                      int fft, int pad, int overlap)
{
    const int col = ui->tblSchedule->columnCount();
    ui->tblSchedule->insertColumn(col);
    ui->tblSchedule->setHorizontalHeaderItem(col, new QTableWidgetItem(name));

    /* "Use in the active schedule" and "delete" are checkboxes, the rest text. */
    QCheckBox *activeBox = new QCheckBox(ui->tblSchedule);
    activeBox->setChecked(active);
    connect(activeBox, SIGNAL(toggled(bool)), this, SLOT(scheduleCheck()));
    ui->tblSchedule->setCellWidget(RowActive, col, activeBox);

    ui->tblSchedule->setItem(RowRep,    col, new QTableWidgetItem(rep));
    ui->tblSchedule->setItem(RowChirpt, col, new QTableWidgetItem(chirpt));
    ui->tblSchedule->setItem(RowRate,   col, new QTableWidgetItem(rate));
    ui->tblSchedule->setItem(RowDur,    col, new QTableWidgetItem(dur));
    ui->tblSchedule->setItem(RowCf,     col, new QTableWidgetItem(cf));
    ui->tblSchedule->setItem(RowLat,    col, new QTableWidgetItem(lat));
    ui->tblSchedule->setItem(RowLon,    col, new QTableWidgetItem(lon));

    QList<int> ffts;
    const QStringList &fftList = FFT_COUNT_LIST();
    for (int i = 0; i < fftList.size(); ++i)
        ffts << fftList.at(i).toInt();
    ui->tblSchedule->setCellWidget(RowFft, col, detailCombo(
        QString::fromUtf8("общ. %1").arg(m_generalFft),
        ffts, QLatin1String("%1"), fft));

    QTableWidgetItem *cell = new QTableWidgetItem();
    cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
    ui->tblSchedule->setItem(RowCell, col, cell);

    ui->tblSchedule->setCellWidget(RowIonoPad, col, detailCombo(
        QString::fromUtf8("общ. ×%1").arg(m_generalPad),
        padChoices(), QString::fromUtf8("×%1"), pad));
    ui->tblSchedule->setCellWidget(RowIonoOverlap, col, detailCombo(
        QString::fromUtf8("общ. ×%1").arg(m_generalOverlap),
        overlapChoices(), QString::fromUtf8("×%1"), overlap));

    QCheckBox *deleteBox = new QCheckBox(ui->tblSchedule);
    connect(deleteBox, SIGNAL(toggled(bool)), this, SLOT(scheduleCheck()));
    ui->tblSchedule->setCellWidget(RowDelete, col, deleteBox);

    updateCell(col);
}

void ScheduleDialog::updateCell(int col)
{
    QTableWidgetItem *cell = ui->tblSchedule->item(RowCell, col);
    QTableWidgetItem *rate = ui->tblSchedule->item(RowRate, col);
    QComboBox *fft = qobject_cast<QComboBox *>(ui->tblSchedule->cellWidget(RowFft, col));
    if (!cell || !rate || !fft)
        return;

    const int n = fft->currentData().toInt() > 0 ? fft->currentData().toInt() : m_generalFft;
    const double rateHz = rate->text().toDouble() * 1e3;
    if (rateHz <= 0.0 || m_ifRateHz <= 0.0 || n <= 0) {
        cell->setText(QString());
        return;
    }
    /* One spectrum integrates n/if_rate seconds: that much sweep, and the
     * reciprocal of it in beat frequency, which is delay over the rate. */
    const double seconds = n / m_ifRateHz;
    const double sweepKHz = rateHz * seconds / 1e3;
    const double delayKm = LIGHT_SPEED_KM_S / (rateHz * seconds);
    cell->setText(QString::fromUtf8("%1 × %2")
                      .arg(sweepKHz, 0, 'f', 0)
                      .arg(delayKm, 0, 'f', 1));
    cell->setToolTip(QString::fromUtf8("%1 кГц развёртки × %2 км задержки, БПФ %3")
                         .arg(sweepKHz, 0, 'f', 0)
                         .arg(delayKm, 0, 'f', 1)
                         .arg(n));
}

void ScheduleDialog::updateCells()
{
    for (int col = 0; col < ui->tblSchedule->columnCount(); ++col)
        updateCell(col);
}

void ScheduleDialog::onCellChanged(int row, int col)
{
    /* The cell row is written by updateCell itself. */
    if (row == RowCell)
        return;
    if (row == RowRate)
        updateCell(col);
    createRxList();
    scheduleCheck();
}

void ScheduleDialog::scheduleCheck()
{
    if (m_checkTimer)
        m_checkTimer->start();
}

QList<ScheduleEntry> ScheduleDialog::tableEntries() const
{
    QList<ScheduleEntry> out;
    const QString rx = ui->cmbRxStation->currentText();
    for (int col = 0; col < ui->tblSchedule->columnCount(); ++col) {
        QTableWidgetItem *head = ui->tblSchedule->horizontalHeaderItem(col);
        if (!head)
            continue;
        QCheckBox *del = qobject_cast<QCheckBox *>(ui->tblSchedule->cellWidget(RowDelete, col));
        if (del && del->isChecked())
            continue;
        QCheckBox *act = qobject_cast<QCheckBox *>(ui->tblSchedule->cellWidget(RowActive, col));
        QComboBox *fft = qobject_cast<QComboBox *>(ui->tblSchedule->cellWidget(RowFft, col));

        ScheduleEntry e;
        e.name   = head->text();
        e.active = act && act->isChecked();
        e.rx     = e.name == rx;
        QString *texts[] = { &e.rep, &e.chirpt, &e.rate, &e.dur, &e.cf, &e.lat, &e.lon };
        const int rows[] = { RowRep, RowChirpt, RowRate, RowDur, RowCf, RowLat, RowLon };
        for (int k = 0; k < 7; ++k) {
            QTableWidgetItem *it = ui->tblSchedule->item(rows[k], col);
            *texts[k] = it ? it->text() : QString();
        }
        e.fft    = fft ? fft->currentData().toInt() : 0;
        out << e;
    }
    return out;
}

void ScheduleDialog::runChecks()
{
    ui->txtIssues->setHtml(issuesHtml(checkSchedule(tableEntries(), m_checkConfig)));
}

void ScheduleDialog::ReadSchedule()
{
    if (!m_settings)
        return;

    ui->tblSchedule->setColumnCount(0);

    QString rxName;
    const QStringList stations = m_settings->childGroups();
    for (int i = 0; i < stations.size(); ++i) {
        const QString &s = stations.at(i);
        m_settings->beginGroup(s);
        addStationColumn(s,
                         m_settings->value(QLatin1String("active")).toBool(),
                         m_settings->value(QLatin1String("rep")).toString(),
                         m_settings->value(QLatin1String("chirpt")).toString(),
                         m_settings->value(QLatin1String("rate")).toString(),
                         m_settings->value(QLatin1String("dur")).toString(),
                         m_settings->value(QLatin1String("cf")).toString(),
                         m_settings->value(QLatin1String("lat")).toString(),
                         m_settings->value(QLatin1String("lon")).toString(),
                         m_settings->value(QLatin1String("fft_count")).toInt(),
                         m_settings->value(QLatin1String("iono_pad")).toInt(),
                         m_settings->value(QLatin1String("iono_overlap")).toInt());
        if (m_settings->value(QLatin1String("rx")).toBool())
            rxName = s;
        m_settings->endGroup();
    }

    createRxList();
    if (!rxName.isEmpty())
        ui->cmbRxStation->setCurrentText(rxName);

    emit scheduleReaded();
}

void ScheduleDialog::WriteSchedule()
{
    if (!m_settings)
        return;

    const QString rx = ui->cmbRxStation->currentText();
    bool deletedAny = false;

    for (int col = 0; col < ui->tblSchedule->columnCount(); ++col) {
        QTableWidgetItem *head = ui->tblSchedule->horizontalHeaderItem(col);
        if (!head)
            continue;
        const QString name = head->text();

        QCheckBox *del = qobject_cast<QCheckBox *>(ui->tblSchedule->cellWidget(RowDelete, col));
        if (del && del->isChecked()) {
            m_settings->remove(name);
            deletedAny = true;
            continue;
        }

        QCheckBox *act = qobject_cast<QCheckBox *>(ui->tblSchedule->cellWidget(RowActive, col));

        m_settings->beginGroup(name);
        m_settings->setValue(QLatin1String("active"), act && act->isChecked());
        const int rows[] = { RowRep, RowChirpt, RowRate, RowDur, RowCf, RowLat, RowLon };
        const char *keys[] = { "rep", "chirpt", "rate", "dur", "cf", "lat", "lon" };
        for (int k = 0; k < 7; ++k) {
            QTableWidgetItem *it = ui->tblSchedule->item(rows[k], col);
            m_settings->setValue(QLatin1String(keys[k]), it ? it->text() : QString());
        }

        /* "General" is the key's absence, not a 0: every key in the group
         * goes into the station's dict in chirp_config.py, and a station with
         * no overrides then writes exactly what it did before they existed. */
        const int detailRows[] = { RowFft, RowIonoPad, RowIonoOverlap };
        const char *detailKeys[] = { "fft_count", "iono_pad", "iono_overlap" };
        for (int k = 0; k < 3; ++k) {
            QComboBox *box = qobject_cast<QComboBox *>(
                ui->tblSchedule->cellWidget(detailRows[k], col));
            const int value = box ? box->currentData().toInt() : 0;
            if (value > 0)
                m_settings->setValue(QLatin1String(detailKeys[k]), value);
            else
                m_settings->remove(QLatin1String(detailKeys[k]));
        }

        m_settings->setValue(QLatin1String("rx"), name == rx);
        m_settings->endGroup();
    }

    m_settings->sync();

    if (deletedAny)
        emit stationsDeleted();
    emit stationEdited();
    emit setRxName(rx);
}

void ScheduleDialog::createRxList()
{
    const QString previous = ui->cmbRxStation->currentText();
    ui->cmbRxStation->clear();
    for (int col = 0; col < ui->tblSchedule->columnCount(); ++col) {
        QTableWidgetItem *head = ui->tblSchedule->horizontalHeaderItem(col);
        if (head)
            ui->cmbRxStation->addItem(head->text());
    }
    if (!previous.isEmpty())
        ui->cmbRxStation->setCurrentText(previous);
}

void ScheduleDialog::on_btnAdd_clicked()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QString::fromUtf8("Расписание"),
        QString::fromUtf8("Название станции:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty())
        return;

    /* newStationParamDefaultValues @0x6cf4a0 in the original. */
    addStationColumn(name, false,
                     QLatin1String("300"), QLatin1String("0"), QLatin1String("100"),
                     QLatin1String("250"), QLatin1String("12500"),
                     QLatin1String("0"), QLatin1String("0"));
    createRxList();
}

void ScheduleDialog::on_btnDelete_clicked()
{
    /* Tick "delete from schedule" for the selected columns; the removal
     * happens on accept, as in the original. */
    QList<QTableWidgetItem *> sel = ui->tblSchedule->selectedItems();
    for (int i = 0; i < sel.size(); ++i) {
        QCheckBox *del = qobject_cast<QCheckBox *>(
            ui->tblSchedule->cellWidget(RowDelete, sel.at(i)->column()));
        if (del)
            del->setChecked(true);
    }
}

void ScheduleDialog::on_buttonBox_accepted()
{
    /* Stop for every error, and for a warning only when this edit brought
     * it: one the operator has already been shown and kept is not news. */
    const QList<CheckIssue> issues = checkSchedule(tableEntries(), m_checkConfig);
    QList<CheckIssue> stop;
    for (int i = 0; i < issues.size(); ++i) {
        const CheckIssue &is = issues.at(i);
        if (is.level == CheckIssue::Error
                || (is.level == CheckIssue::Warning
                    && !m_initialIssues.contains(issueLine(is))))
            stop << is;
    }
    if (!stop.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, windowTitle(),
                        (countIssues(stop, CheckIssue::Error) > 0
                             ? QString::fromUtf8("В расписании есть ошибки.")
                             : QString::fromUtf8("После правки появились замечания."))
                            + QLatin1String("<br><br>") + issuesHtml(stop),
                        QMessageBox::NoButton, this);
        box.setTextFormat(Qt::RichText);
        QPushButton *save = box.addButton(QString::fromUtf8("Сохранить как есть"),
                                          QMessageBox::AcceptRole);
        QPushButton *back = box.addButton(QString::fromUtf8("Вернуться к правке"),
                                          QMessageBox::RejectRole);
        box.setDefaultButton(back);
        box.exec();
        if (box.clickedButton() != save) {
            runChecks();
            return;
        }
    }
    WriteSchedule();
    accept();
}

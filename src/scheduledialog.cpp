#include "scheduledialog.h"
#include "ui_scheduledialog.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QTableWidget>
#include <QTableWidgetItem>

ScheduleDialog::ScheduleDialog(QSettings *scheduleSettings, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ScheduleDialog),
      m_settings(scheduleSettings)
{
    ui->setupUi(this);

    QStringList headers;
    headers << QString::fromUtf8("Использовать в активном расписании")
            << QString::fromUtf8("Период повторения, с")
            << QString::fromUtf8("chirptime, c")
            << QString::fromUtf8("Скорость перестройки ЛЧМ сигнала, кГц/с")
            << QString::fromUtf8("Продолжительность сеанса, с")
            << QString::fromUtf8("Частота программного гетеродина USRP, кГц")
            << QString::fromUtf8("Широта (Ю-, С+)")
            << QString::fromUtf8("Долгота (З-, В+)")
            << QString::fromUtf8("Удалить из расписания");

    ui->tblSchedule->setRowCount(RowCount);
    ui->tblSchedule->setVerticalHeaderLabels(headers);
    ui->tblSchedule->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    connect(ui->tblSchedule, SIGNAL(cellChanged(int,int)), this, SLOT(createRxList()));

    ReadSchedule();
}

ScheduleDialog::~ScheduleDialog()
{
    delete ui;
}

void ScheduleDialog::addStationColumn(const QString &name, bool active, const QString &rep,
                                      const QString &chirpt, const QString &rate,
                                      const QString &dur, const QString &cf,
                                      const QString &lat, const QString &lon)
{
    const int col = ui->tblSchedule->columnCount();
    ui->tblSchedule->insertColumn(col);
    ui->tblSchedule->setHorizontalHeaderItem(col, new QTableWidgetItem(name));

    /* "Use in the active schedule" and "delete" are checkboxes, the rest text. */
    QCheckBox *activeBox = new QCheckBox(ui->tblSchedule);
    activeBox->setChecked(active);
    ui->tblSchedule->setCellWidget(RowActive, col, activeBox);

    ui->tblSchedule->setItem(RowRep,    col, new QTableWidgetItem(rep));
    ui->tblSchedule->setItem(RowChirpt, col, new QTableWidgetItem(chirpt));
    ui->tblSchedule->setItem(RowRate,   col, new QTableWidgetItem(rate));
    ui->tblSchedule->setItem(RowDur,    col, new QTableWidgetItem(dur));
    ui->tblSchedule->setItem(RowCf,     col, new QTableWidgetItem(cf));
    ui->tblSchedule->setItem(RowLat,    col, new QTableWidgetItem(lat));
    ui->tblSchedule->setItem(RowLon,    col, new QTableWidgetItem(lon));

    ui->tblSchedule->setCellWidget(RowDelete, col, new QCheckBox(ui->tblSchedule));
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
                         m_settings->value(QLatin1String("lon")).toString());
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
    WriteSchedule();
    accept();
}

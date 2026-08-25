#ifndef SCHEDULEDIALOG_H
#define SCHEDULEDIALOG_H

#include <QDialog>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace Ui { class ScheduleDialog; }

/*
 * ScheduleDialog @0x461570
 *
 * moc signals: stationsDeleted(), stationEdited(), scheduleReaded(),
 *              setRxName(rxName)
 * moc slots:   on_btnAdd_clicked, on_btnDelete_clicked,
 *              on_buttonBox_accepted, createRxList()
 *
 * The table is transposed relative to the .ini: one COLUMN per station, one
 * ROW per parameter, which is how the manual's screenshot shows it.
 */
class ScheduleDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ScheduleDialog(QSettings *scheduleSettings, QWidget *parent = 0);
    ~ScheduleDialog();

    void ReadSchedule();
    void WriteSchedule();

signals:
    void stationsDeleted();
    void stationEdited();
    void scheduleReaded();
    void setRxName(const QString &rxName);

private slots:
    void on_btnAdd_clicked();
    void on_btnDelete_clicked();
    void on_buttonBox_accepted();
    void createRxList();

private:
    /* Row order of the parameter table. */
    enum Row {
        RowActive = 0, RowRep, RowChirpt, RowRate, RowDur, RowCf,
        RowLat, RowLon, RowDelete, RowCount
    };

    void addStationColumn(const QString &name, bool active, const QString &rep,
                          const QString &chirpt, const QString &rate,
                          const QString &dur, const QString &cf,
                          const QString &lat, const QString &lon);

    Ui::ScheduleDialog *ui;
    QSettings *m_settings;
};

#endif /* SCHEDULEDIALOG_H */

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
    void onCellChanged(int row, int col);
    void updateCells();

private:
    /* Row order of the parameter table. The rows from RowFft on are not in
     * the original: per-station overrides of the parameters dialog, kept out
     * of schedule.ini altogether while they say "general". */
    enum Row {
        RowActive = 0, RowRep, RowChirpt, RowRate, RowDur, RowCf,
        RowLat, RowLon, RowFft, RowCell, RowIonoPad, RowIonoOverlap,
        RowDelete, RowCount
    };

    void addStationColumn(const QString &name, bool active, const QString &rep,
                          const QString &chirpt, const QString &rate,
                          const QString &dur, const QString &cf,
                          const QString &lat, const QString &lon,
                          int fft = 0, int pad = 0, int overlap = 0);
    /* A combo whose first entry, data 0, means "as in the parameters dialog". */
    class QComboBox *detailCombo(const QString &general, const QList<int> &values,
                                 const QString &format, int current);
    /* The frequency x delay cell this column's rate and FFT length give. */
    void updateCell(int col);

    Ui::ScheduleDialog *ui;
    QSettings *m_settings;

    /* The parameters dialog's values, which a station without its own uses. */
    int    m_generalFft;
    int    m_generalPad;
    int    m_generalOverlap;
    double m_ifRateHz;
};

#endif /* SCHEDULEDIALOG_H */

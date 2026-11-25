#include "../src/scheduledialog.h"

#include <QApplication>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cstdio>

static bool theScheduleDialogKeepsTheSweepStartAfterSaving()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QLatin1String("schedule.ini"));
    {
        QSettings schedule(path, QSettings::IniFormat);
        schedule.setValue(QLatin1String("NIC2/f0"), QLatin1String("7.5"));
        ScheduleDialog dialog(&schedule);
        QTableWidget *table = dialog.findChild<QTableWidget *>(QLatin1String("tblSchedule"));
        for (int row = 0; table && row < table->rowCount(); ++row) {
            if (table->verticalHeaderItem(row)->text() == QString::fromUtf8("Начало свипа, МГц")) {
                QTableWidgetItem *field = table->item(row, 0);
                if (field && field->text() == QLatin1String("7.5"))
                    field->setText(QLatin1String("7.525"));
                break;
            }
        }
        dialog.WriteSchedule();
    }

    QSettings reopened(path, QSettings::IniFormat);
    const bool ok = reopened.value(QLatin1String("NIC2/f0")).toString() == QLatin1String("7.525");
    if (!ok)
        std::printf("FAIL: the schedule dialog did not preserve the edited sweep start in the ini file.\n");
    return ok;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    return theScheduleDialogKeepsTheSweepStartAfterSaving() ? 0 : 1;
}

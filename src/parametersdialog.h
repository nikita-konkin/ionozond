#ifndef PARAMETERSDIALOG_H
#define PARAMETERSDIALOG_H

#include <QComboBox>
#include <QDialog>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace Ui { class ParametersDialog; }

/*
 * ParametersDialog @0x459b40
 *
 * moc signals: changeSoundAppFileName(fileName), changeConfigFileName(fileName),
 *              igDirChanged(filename)
 * moc slots:   on_btbOkCancel_accepted, on_btbOkCancel_rejected,
 *              on_btnSoundApp_clicked, on_btnPyConfig_clicked, on_btnIGDir_clicked
 *
 * frmMain only learns the sounder and config paths through these signals, which
 * is why nothing works until the dialog has been accepted once -- see NOTES.md.
 */
class ParametersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParametersDialog(QSettings *settings, QWidget *parent = 0);
    ~ParametersDialog();

    void ReadSettings();
    void WriteSettings();

signals:
    void changeSoundAppFileName(const QString &fileName);
    void changeConfigFileName(const QString &fileName);
    void igDirChanged(const QString &filename);

private slots:
    void on_btbOkCancel_accepted();
    void on_btbOkCancel_rejected();
    void on_btnSoundApp_clicked();
    void on_btnPyConfig_clicked();
    void on_btnIGDir_clicked();
    /* Restates the speckle threshold as a fraction of the window, and warns
     * when it is high enough to take the trace with the noise. */
    void UpdateObjLevelHint();

private:
    static void SetParamsToComboBox(QComboBox *box, const QStringList &items, const int &index);

    Ui::ParametersDialog *ui;
    QSettings *m_settings;
};

#endif /* PARAMETERSDIALOG_H */

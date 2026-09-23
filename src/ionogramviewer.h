#ifndef IONOGRAMVIEWER_H
#define IONOGRAMVIEWER_H

#include "common.h"

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QToolButton;
class QIGFrame;
class QRxIonogram;
class IgZoomer;

/*
 * One ionogram in a window of its own.
 *
 * The console gives each station a quarter of its height, so with two
 * stations an ionogram is about 90 px tall -- enough to see that a trace is
 * there, not enough to read it. This opens the same capture full size, as a
 * live plot rather than a picture: drag to zoom, right-click to zoom back,
 * the cursor reads out frequency and delay, and the arrows walk the
 * station's captures the same way the control panel does.
 *
 * Owned by the station's QIGFrame, so it closes when the console rebuilds
 * its stations rather than outliving the list it walks.
 */
class IonogramViewer : public QWidget
{
    Q_OBJECT

public:
    IonogramViewer(const QBaseSoundParams &base,
                   const QTxParams &tx,
                   const QRxParams &rx,
                   QIGFrame *frame,
                   const QString &capture,
                   bool follow);

public slots:
    /* The frame's capture list changed: follow the newest if asked to,
     * otherwise just keep the position readout honest. */
    void onHistoryChanged();

private slots:
    void back();
    void forward();
    void latest();
    void savePng();
    void followToggled(bool on);

private:
    void showCapture(const QString &capture);
    void updateNav();

    QIGFrame    *m_frame;
    QTxParams    m_tx;
    QRxIonogram *m_plot;
    IgZoomer    *m_zoomer;
    QString      m_capture;

    QLabel      *m_pos;
    QLabel      *m_file;
    QToolButton *m_btnBack;
    QToolButton *m_btnForward;
    QToolButton *m_btnLatest;
    QCheckBox   *m_follow;
};

#endif /* IONOGRAMVIEWER_H */

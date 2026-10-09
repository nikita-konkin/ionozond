#ifndef QIGFRAME_H
#define QIGFRAME_H

#include "common.h"
#include "liveionogram.h"
#include "qrxionogram.h"
#include "snrvariationswidget.h"
#include "pdpvariationswidget.h"

#include <QDateTime>
#include <QStringList>
#include <QFrame>
#include <QLabel>
#include <QString>

class QGridLayout;

/*
 * QIGFrame @0x466f80 -- the per-station panel group.
 *
 * Layout, from the original's screenshots: a black title bar carrying the
 * transmitter name, then a 2x2 grid of
 *
 *   Контрольная ионограмма | Сигнал/шум
 *   Текущая ионограмма     | ПЗМ
 *
 * The two right-hand panels (SNR and power-delay-profile variations) are
 * placeholders until SnrVariationsWidget / PdpVariationsWidget are written.
 */
class QIGFrame : public QFrame
{
    Q_OBJECT

public:
    QIGFrame(const QBaseSoundParams &base,
             const QTxParams &tx,
             const QRxParams &rx,
             const SnrVarParams &snrParams = SnrVarParams(),
             const PdpVarParams &pdpParams = PdpVarParams(),
             QWidget *parent = 0);
    ~QIGFrame();

    QString stationName() const { return m_tx.name; }

    /* Load a capture into the "current" panel; the previous current becomes
     * the control panel, which is how the original's two-panel view works. */
    /* `keepControl` false when replaying archive history: only the last two
     * captures are ever shown, so re-rendering each one into the control panel
     * on the way past doubles the work for nothing. */
    bool addIg(const QString &igFileName, bool keepControl = true);

    QRxIonogram *controlIonogram() const { return m_control; }
    QRxIonogram *currentIonogram() const { return m_current; }
    SnrVariationsWidget *snrWidget() const { return m_snr; }
    PdpVariationsWidget *pdpWidget() const { return m_pdp; }

    /* Every capture loaded so far, oldest first. */
    const QStringList &history() const { return m_history; }

    /*
     * The sounding being recorded, drawn into the current panel as it
     * arrives; see liveionogram.h. Called on every live tick with the path of
     * the station's newest unfinished capture. When it starts, the current
     * panel's capture moves to the control panel, as addIg does for a
     * finished one -- 250 s sooner. addIg with the same path ends it.
     */
    void followLive(const QString &lfsPath);
    /* The capture being followed has gone without finishing (the sounder
     * stopped early): put the panels back as they were. */
    void dropLive();
    const LiveIonogram *live() const { return m_live; }

signals:
    /* A capture was added, or the list was cleared. */
    void historyChanged();
    /* More of the sounding being recorded has been drawn. */
    void liveUpdated(const LiveIonogram *live);
    /* The live sounding was dropped without a finished capture. */
    void liveEnded();

public slots:
    void setIgAreaVisible(const QString &stationName, const bool &visible);
    void clear();

protected:
    /* Double-click on either ionogram opens it in a window of its own. */
    bool eventFilter(QObject *obj, QEvent *ev);

private slots:
    void popoutControl();
    void popoutCurrent();
    /* Step the control panel through the captures already loaded. Stepping
     * past the newest returns it to following, which is where it starts. */
    void controlBack();
    void controlForward();
    void controlLatest();

private:
    QLabel *makeCaption(const QString &text) const;
    QWidget *makeControlHeader();
    QWidget *makeCurrentHeader();
    class QToolButton *makePopoutButton(QWidget *parent) const;

    /* `follow`: the window moves on to each new capture as it arrives.
     * True from the current panel, false from the control panel, which
     * is showing an older capture on purpose. */
    void openViewer(QRxIonogram *panel, bool follow);

    LiveSettings liveSettings() const;
    /* Hand a loaded capture's SNR and PDP to the variation panels. */
    void feedVariations(const QRxIonogram *panel);

    void showControlAt(int index);
    void updateControlNav();
    /* Which capture the control panel is showing: the pinned one, or the
     * one before the current when following. */
    int  effectiveControlIndex() const;

    QBaseSoundParams m_base;
    QTxParams        m_tx;
    QRxParams        m_rx;

    SnrVarParams m_snrParams;
    PdpVarParams m_pdpParams;

    QRxIonogram         *m_control;
    QRxIonogram         *m_current;
    SnrVariationsWidget *m_snr;
    PdpVariationsWidget *m_pdp;

    /* Every capture handed to addIg, oldest first, so the control panel can
     * walk back through the day instead of only ever showing the previous
     * one. Paths only -- the rasters are loaded on demand. */
    QStringList m_history;
    int         m_controlIndex;      /* -1: follow the newest */

    QLabel            *m_controlPos;
    class QToolButton *m_btnBack;
    class QToolButton *m_btnForward;
    class QToolButton *m_btnLatest;

    LiveIonogram *m_live;            /* the sounding being recorded, if any */
};

#endif /* QIGFRAME_H */

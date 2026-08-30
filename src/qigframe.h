#ifndef QIGFRAME_H
#define QIGFRAME_H

#include "common.h"
#include "qrxionogram.h"
#include "snrvariationswidget.h"
#include "pdpvariationswidget.h"

#include <QDateTime>
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

public slots:
    void setIgAreaVisible(const QString &stationName, const bool &visible);
    void clear();

private:
    QLabel *makeCaption(const QString &text) const;

    QBaseSoundParams m_base;
    QTxParams        m_tx;
    QRxParams        m_rx;

    SnrVarParams m_snrParams;
    PdpVarParams m_pdpParams;

    QRxIonogram         *m_control;
    QRxIonogram         *m_current;
    SnrVariationsWidget *m_snr;
    PdpVariationsWidget *m_pdp;
};

#endif /* QIGFRAME_H */

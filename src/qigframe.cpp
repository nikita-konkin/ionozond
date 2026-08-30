#include "qigframe.h"

#include "iganalytics.h"

#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

QIGFrame::QIGFrame(const QBaseSoundParams &base,
                   const QTxParams &tx,
                   const QRxParams &rx,
                   const SnrVarParams &snrParams,
                   const PdpVarParams &pdpParams,
                   QWidget *parent)
    : QFrame(parent),
      m_base(base),
      m_tx(tx),
      m_rx(rx),
      m_snrParams(snrParams),
      m_pdpParams(pdpParams),
      m_control(0),
      m_current(0),
      m_snr(0),
      m_pdp(0),
      m_controlIndex(-1),
      m_controlPos(0),
      m_btnBack(0),
      m_btnForward(0),
      m_btnLatest(0)
{
    setFrameShape(QFrame::StyledPanel);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    /* Title bar: white bold station name on black, as in the original. */
    QLabel *title = new QLabel(m_tx.name, this);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QLatin1String(
        "background-color: rgb(0, 0, 0); color: rgb(255, 255, 255); font-weight: bold;"));
    title->setMinimumHeight(20);
    outer->addWidget(title);

    QGridLayout *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(6);

    m_control = new QRxIonogram(m_base, m_tx, m_rx, this);
    m_control->setControlIonogram(true);
    m_current = new QRxIonogram(m_base, m_tx, m_rx, this);

    /*
     * The variation panels span the whole sweep and the whole delay window,
     * taken from the ionogram panel's own axes so all four agree.
     */
    const QIgWindow freq = m_current->horizontalInterval();
    const QIgWindow delay = m_current->verticalInterval();

    /* The variation plots use colormap_index, NOT ig_colormap_index -- see
     * NOTES.md; the shipped value 8 is IG2_MOD_BASE_COLORS. */
    const QVector<QColorLevel> &varColors = colorLevelsForIndex(m_base.colormapIndex);

    m_snr = new SnrVariationsWidget(m_snrParams.periodHour,
                                    m_snrParams.timeBandMin,
                                    (float)freq.left(), (float)freq.right(),
                                    varColors, m_snrParams.autoMax, 0.0f, this);

    m_pdp = new PdpVariationsWidget(m_pdpParams.periodHour,
                                    m_pdpParams.timeBandMin,
                                    (float)delay.top(), (float)delay.bottom(),
                                    varColors, m_pdpParams.autoMax, 0.0f, this);

    grid->addWidget(makeControlHeader(), 0, 0);
    grid->addWidget(makeCaption(QString::fromUtf8("Сигнал/шум")),             0, 1);
    grid->addWidget(m_control, 1, 0);
    grid->addWidget(m_snr,     1, 1);

    grid->addWidget(makeCaption(QString::fromUtf8("Текущая ионограмма")), 2, 0);
    grid->addWidget(makeCaption(QString::fromUtf8("ПЗМ")),                2, 1);
    grid->addWidget(m_current, 3, 0);
    grid->addWidget(m_pdp,     3, 1);

    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(1, 1);
    grid->setRowStretch(3, 1);

    outer->addLayout(grid);
}

/*
 * The control panel's caption, with the controls for walking back through the
 * archive beside it.
 *
 * The two panels only ever showed the newest capture and the one before it, so
 * anything older could be looked at solely by rebuilding the whole console
 * against a different period. Everything needed was already loaded -- the
 * variation panels are fed from the same files -- so this is navigation over a
 * list the frame was keeping anyway.
 */
QWidget *QIGFrame::makeControlHeader()
{
    QWidget *row = new QWidget(this);
    QHBoxLayout *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(3);

    lay->addWidget(makeCaption(QString::fromUtf8("Контрольная ионограмма")), 1);

    m_controlPos = new QLabel(row);
    m_controlPos->setStyleSheet(QLatin1String("color: #808080;"));
    lay->addWidget(m_controlPos);

    /* Compact, because this sits on a caption line 18 px tall. */
    m_btnBack = new QToolButton(row);
    m_btnBack->setText(QChar(0x25C0));       /* black left triangle */
    m_btnBack->setToolTip(QString::fromUtf8("Предыдущая ионограмма"));
    m_btnBack->setAutoRepeat(true);
    m_btnBack->setFixedSize(22, 18);

    m_btnForward = new QToolButton(row);
    m_btnForward->setText(QChar(0x25B6));    /* black right triangle */
    m_btnForward->setToolTip(QString::fromUtf8("Следующая ионограмма"));
    m_btnForward->setAutoRepeat(true);
    m_btnForward->setFixedSize(22, 18);

    m_btnLatest = new QToolButton(row);
    m_btnLatest->setText(QChar(0x21BA));     /* anticlockwise arrow */
    m_btnLatest->setToolTip(QString::fromUtf8(
        "Вернуться к слежению за последними сеансами"));
    m_btnLatest->setFixedSize(22, 18);

    connect(m_btnBack,    SIGNAL(clicked()), this, SLOT(controlBack()));
    connect(m_btnForward, SIGNAL(clicked()), this, SLOT(controlForward()));
    connect(m_btnLatest,  SIGNAL(clicked()), this, SLOT(controlLatest()));

    lay->addWidget(m_btnBack);
    lay->addWidget(m_btnForward);
    lay->addWidget(m_btnLatest);

    updateControlNav();
    return row;
}

int QIGFrame::effectiveControlIndex() const
{
    if (m_controlIndex >= 0)
        return m_controlIndex;
    /* Following: the control panel holds the one before the current. */
    return qMax(0, m_history.size() - 2);
}

void QIGFrame::showControlAt(int index)
{
    if (m_history.isEmpty())
        return;
    index = qBound(0, index, m_history.size() - 1);

    /* Stepping onto the newest is the same thing as following it, so let that
     * release the pin rather than leaving the operator stuck one behind with
     * no indication why new captures stopped arriving. */
    if (index >= m_history.size() - 1) {
        controlLatest();
        return;
    }

    m_controlIndex = index;
    m_control->load(m_history.at(index));
    updateControlNav();
}

void QIGFrame::controlBack()    { showControlAt(effectiveControlIndex() - 1); }
void QIGFrame::controlForward() { showControlAt(effectiveControlIndex() + 1); }

void QIGFrame::controlLatest()
{
    m_controlIndex = -1;
    if (m_history.size() >= 2)
        m_control->load(m_history.at(m_history.size() - 2));
    updateControlNav();
}

void QIGFrame::updateControlNav()
{
    if (!m_controlPos)
        return;

    const int n = m_history.size();
    const int at = effectiveControlIndex();

    if (n == 0) {
        m_controlPos->setText(QString());
    } else if (m_controlIndex >= 0) {
        /* Pinned: say so, because new captures will not move this panel and
         * that is otherwise indistinguishable from the sounder having stopped. */
        m_controlPos->setText(QString(QLatin1String("%1/%2 "))
                                  .arg(at + 1).arg(n)
                              + QChar(0x25CF));   /* pinned */
        m_controlPos->setToolTip(QString::fromUtf8("Закреплено: ")
                                 + QFileInfo(m_history.at(at)).fileName());
    } else {
        m_controlPos->setText(QString(QLatin1String("%1/%2")).arg(at + 1).arg(n));
        m_controlPos->setToolTip(QString::fromUtf8("Следит за последними сеансами"));
    }

    if (m_btnBack)    m_btnBack->setEnabled(n > 1 && at > 0);
    if (m_btnForward) m_btnForward->setEnabled(n > 1 && at < n - 1);
    if (m_btnLatest)  m_btnLatest->setEnabled(m_controlIndex >= 0);
}

QLabel *QIGFrame::makeCaption(const QString &text) const
{
    QLabel *l = new QLabel(text, const_cast<QIGFrame *>(this));
    l->setMaximumHeight(18);
    return l;
}

bool QIGFrame::addIg(const QString &igFileName, bool keepControl)
{
    /*
     * The panel that was "current" becomes the control ionogram, and the new
     * capture takes the current slot.
     */
    /* A pinned control panel stays where the operator put it. Promoting the
     * previous capture into it on every arrival is the following behaviour,
     * and following is what pinning switches off. */
    if (keepControl && m_controlIndex < 0 && !m_current->igFileName().isEmpty())
        m_control->load(m_current->igFileName());

    /* Sidecar-first: skips 80 MB and 610 FFTs when one exists, and writes one
     * for next time when it does not. See docs/lfp-format.md. */
    if (!m_current->load(igFileName))
        return false;

    /* Oldest first, and only once: LoadLatestCaptures replays the archive in
     * order and ScanForNewCaptures appends, so a duplicate means the same file
     * arrived twice and the list should not grow for it. */
    if (m_history.isEmpty() || m_history.last() != igFileName)
        m_history.append(igFileName);
    updateControlNav();

    /*
     * Feed the derived products into the two variation panels, averaging the
     * SNR over the configured frequency band ("усреднение по частоте").
     */
    const QIgWindow freq = m_current->horizontalInterval();
    const QVector<float> snr = averageSnrOverFreqBands(
        m_current->snrVector(),
        freq.right() - freq.left(),
        m_snrParams.freqBandKHz);

    m_snr->onValueAdded(m_current->startDateTime(), snr,
                        m_current->luf_MHz(),
                        m_current->muf_MHz());

    m_pdp->onValueAdded(m_current->startDateTime(), m_current->pdpVector());
    return true;
}

void QIGFrame::setIgAreaVisible(const QString &stationName, const bool &visible)
{
    if (stationName == m_tx.name)
        setVisible(visible);
}

void QIGFrame::clear()
{
    m_history.clear();
    m_controlIndex = -1;
    updateControlNav();
    if (m_control) m_control->clear();
    if (m_current) m_current->clear();
    if (m_snr)     m_snr->clear();
    if (m_pdp)     m_pdp->clear();
}

#include "qigframe.h"

#include "iganalytics.h"

#include <QGridLayout>
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
      m_pdp(0)
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

    grid->addWidget(makeCaption(QString::fromUtf8("Контрольная ионограмма")), 0, 0);
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

QLabel *QIGFrame::makeCaption(const QString &text) const
{
    QLabel *l = new QLabel(text, const_cast<QIGFrame *>(this));
    l->setMaximumHeight(18);
    return l;
}

bool QIGFrame::addIg(const QString &igFileName)
{
    /*
     * The panel that was "current" becomes the control ionogram, and the new
     * capture takes the current slot.
     */
    if (!m_current->igFileName().isEmpty())
        m_control->load(m_current->igFileName());

    /* Sidecar-first: skips 80 MB and 610 FFTs when one exists, and writes one
     * for next time when it does not. See docs/lfp-format.md. */
    if (!m_current->load(igFileName))
        return false;

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
    if (m_control) m_control->clear();
    if (m_current) m_current->clear();
    if (m_snr)     m_snr->clear();
    if (m_pdp)     m_pdp->clear();
}

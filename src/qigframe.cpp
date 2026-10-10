#include "qigframe.h"

#include "iganalytics.h"
#include "ionogramviewer.h"

#include <QEvent>
#include <QFileInfo>
#include <QScopedPointer>

/* The original's FFT length, which ПЗМ values are scaled to. */
static const int PDP_REFERENCE_FFT = 16384;
#include <QGridLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <qwt_plot_canvas.h>
/* Qwt 6.1's header uses a QFlags constructor newer Qt deprecates. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <qwt_plot_layout.h>
#pragma GCC diagnostic pop
#include <qwt_scale_widget.h>

/*
 * Pull a panel's decorations in to its canvas. A station frame puts four
 * plots in a third or half of the screen, and the stock Qwt spacing -- a
 * margin round the canvas, a gap before every scale, a 2 px sunken frame --
 * was taking a fifth of that from the data. `xTitle` false drops the bottom
 * axis title, for a top-row plot whose axis is repeated, titled, under the
 * plot below it.
 */
static void tighten(QwtPlot *plot, bool xTitle)
{
    plot->setContentsMargins(0, 0, 0, 0);
    plot->plotLayout()->setCanvasMargin(0);
    plot->plotLayout()->setSpacing(1);
    plot->plotLayout()->setAlignCanvasToScales(true);
    for (int axis = 0; axis < QwtPlot::axisCnt; ++axis) {
        QwtScaleWidget *scale = plot->axisWidget(axis);
        if (!scale)
            continue;
        scale->setMargin(0);
        scale->setSpacing(1);
    }
    if (QwtPlotCanvas *canvas = qobject_cast<QwtPlotCanvas *>(plot->canvas())) {
        canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
        canvas->setLineWidth(1);
    }
    if (!xTitle)
        plot->setAxisTitle(QwtPlot::xBottom, QwtText());
    plot->updateLayout();
}

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
      m_btnLatest(0),
      m_live(0),
      m_controlCaption(0),
      m_currentCaption(0)
{
    setFrameShape(QFrame::StyledPanel);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(2, 2, 2, 2);
    outer->setSpacing(2);

    /* Title bar: white bold station name on black, as in the original. */
    QLabel *title = new QLabel(m_tx.name, this);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QLatin1String(
        "background-color: rgb(0, 0, 0); color: rgb(255, 255, 255); font-weight: bold;"));
    title->setMinimumHeight(20);
    outer->addWidget(title);

    QGridLayout *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(1);

    m_control = new QRxIonogram(m_base, m_tx, m_rx, this);
    m_control->setControlIonogram(true);
    m_current = new QRxIonogram(m_base, m_tx, m_rx, this);
    connect(m_control, SIGNAL(titleChanged(QString)), this, SLOT(showControlTitle(QString)));
    connect(m_current, SIGNAL(titleChanged(QString)), this, SLOT(showCurrentTitle(QString)));

    /* With two stations each panel is about 90 px tall: enough to see that
     * there is a trace, not to read it. Double-click opens it full size. */
    const QString popoutTip = QString::fromUtf8(
        "Двойной щелчок — открыть ионограмму в отдельном окне");
    m_control->canvas()->installEventFilter(this);
    m_current->canvas()->installEventFilter(this);
    m_control->canvas()->setToolTip(popoutTip);
    m_current->canvas()->setToolTip(popoutTip);

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

    grid->addWidget(makeCurrentHeader(), 2, 0);
    grid->addWidget(makeCaption(QString::fromUtf8("ПЗМ")),                2, 1);
    grid->addWidget(m_current, 3, 0);
    grid->addWidget(m_pdp,     3, 1);

    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(1, 1);
    grid->setRowStretch(3, 1);

    /* The top row's x axes are the bottom row's again: frequency over
     * frequency, UTC over UTC. Their titles are said once, underneath. */
    m_control->setCompact(true);
    m_current->setCompact(true);
    tighten(m_control, false);
    tighten(m_snr, false);
    tighten(m_current, true);
    tighten(m_pdp, true);

    outer->addLayout(grid);
}

static QString captionWithTitle(const char *caption, const QString &title)
{
    QString text = QString::fromUtf8(caption).toHtmlEscaped();
    if (!title.isEmpty())
        text += QLatin1String("&nbsp;&nbsp;<b>") + title.toHtmlEscaped()
              + QLatin1String("</b>");
    return text;
}

void QIGFrame::showControlTitle(const QString &title)
{
    if (m_controlCaption)
        m_controlCaption->setText(captionWithTitle("Контрольная ионограмма", title));
}

void QIGFrame::showCurrentTitle(const QString &title)
{
    if (m_currentCaption)
        m_currentCaption->setText(captionWithTitle("Текущая ионограмма", title));
}

QIGFrame::~QIGFrame()
{
    delete m_live;
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

    m_controlCaption = makeCaption(QString::fromUtf8("Контрольная ионограмма"));
    m_controlCaption->setTextFormat(Qt::RichText);
    lay->addWidget(m_controlCaption, 1);

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

    QToolButton *popout = makePopoutButton(row);
    connect(popout, SIGNAL(clicked()), this, SLOT(popoutControl()));
    lay->addWidget(popout);

    updateControlNav();
    return row;
}

QWidget *QIGFrame::makeCurrentHeader()
{
    QWidget *row = new QWidget(this);
    QHBoxLayout *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(3);

    m_currentCaption = makeCaption(QString::fromUtf8("Текущая ионограмма"));
    m_currentCaption->setTextFormat(Qt::RichText);
    lay->addWidget(m_currentCaption, 1);

    QToolButton *popout = makePopoutButton(row);
    connect(popout, SIGNAL(clicked()), this, SLOT(popoutCurrent()));
    lay->addWidget(popout);
    return row;
}

QToolButton *QIGFrame::makePopoutButton(QWidget *parent) const
{
    QToolButton *b = new QToolButton(parent);
    b->setText(QChar(0x2922));   /* north-east and south-west arrow */
    b->setToolTip(QString::fromUtf8("Открыть в отдельном окне"));
    b->setFixedSize(22, 18);
    return b;
}

void QIGFrame::openViewer(QRxIonogram *panel, bool follow)
{
    const QString capture = panel->igFileName();
    if (capture.isEmpty())
        return;   /* nothing loaded in that panel yet */

    IonogramViewer *viewer = new IonogramViewer(m_base, m_tx, m_rx, this,
                                                capture, follow);
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
}

void QIGFrame::popoutControl() { openViewer(m_control, false); }
void QIGFrame::popoutCurrent() { openViewer(m_current, true); }

bool QIGFrame::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::MouseButtonDblClick) {
        if (m_control && obj == m_control->canvas()) {
            popoutControl();
            return true;
        }
        if (m_current && obj == m_current->canvas()) {
            popoutCurrent();
            return true;
        }
    }
    return QFrame::eventFilter(obj, ev);
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
    /* With the capture time in UTC and Moscow time the ionogram captions run
     * long; in a narrow window they are cut off rather than widening it. */
    l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return l;
}

bool QIGFrame::addIg(const QString &igFileName, bool keepControl)
{
    QRxIonogram *panel = m_current;
    QScopedPointer<QRxIonogram> scratch;

    if (m_live && igFileName == m_live->path()) {
        /* The sounding that was drawn live has finished: its own picture
         * replaces the live one. The control panel moved on when it began. */
        delete m_live;
        m_live = 0;
    } else if (m_live) {
        /*
         * An older sounding whose sidecar arrived after the next one had begun
         * -- back-to-back soundings leave the scan no gap to load it in. It
         * belongs behind the live one: in the control panel when that is
         * following, and nowhere visible when it is pinned, where it is
         * loaded only for the variation panels.
         */
        if (m_controlIndex < 0) {
            panel = m_control;
        } else {
            scratch.reset(new QRxIonogram(m_base, m_tx, m_rx));
            panel = scratch.data();
        }
    } else if (keepControl && m_controlIndex < 0 && !m_current->igFileName().isEmpty()) {
        /*
         * The panel that was "current" becomes the control ionogram, and the
         * new capture takes the current slot.
         *
         * A pinned control panel stays where the operator put it. Promoting
         * the previous capture into it on every arrival is the following
         * behaviour, and following is what pinning switches off.
         */
        m_control->load(m_current->igFileName());
    }

    /* Sidecar-first: skips 80 MB and 610 FFTs when one exists, and writes one
     * for next time when it does not. See docs/lfp-format.md. */
    if (!panel->load(igFileName))
        return false;

    /* Oldest first, and only once: LoadLatestCaptures replays the archive in
     * order and ScanForNewCaptures appends, so a duplicate means the same file
     * arrived twice and the list should not grow for it. */
    if (m_history.isEmpty() || m_history.last() != igFileName)
        m_history.append(igFileName);
    updateControlNav();
    emit historyChanged();

    feedVariations(panel);
    return true;
}

void QIGFrame::feedVariations(const QRxIonogram *panel)
{
    /*
     * Feed the derived products into the two variation panels, averaging the
     * SNR over the configured frequency band ("усреднение по частоте").
     */
    const QIgWindow freq = panel->horizontalInterval();
    const QVector<float> snr = averageSnrOverFreqBands(
        panel->snrVector(),
        freq.right() - freq.left(),
        m_snrParams.freqBandKHz);

    m_snr->onValueAdded(panel->startDateTime(), snr,
                        panel->luf_MHz(),
                        panel->muf_MHz());

    /*
     * ПЗМ sums the in-band spectra, so its size went with how many there are:
     * a station on 4096 points has four times as many as on 16384, and its
     * column came out four times as large, rescaling the whole panel. Each
     * spectrum now counts for its length relative to 16384 points -- the
     * original's -- so stations on 16384 read exactly as before, and one
     * moved to another length does not jump.
     */
    PdpVector pdp = panel->pdpVector();
    const int points = panel->analysisFftCount();
    if (points > 0 && points != PDP_REFERENCE_FFT) {
        const double weight = (double)points / (double)PDP_REFERENCE_FFT;
        for (int i = 0; i < pdp.size(); ++i)
            pdp[i].value *= weight;
    }
    m_pdp->onValueAdded(panel->startDateTime(), pdp);
}

LiveSettings QIGFrame::liveSettings() const
{
    /* m_base is already this station's: its FFT length and detail. */
    LiveSettings s;
    s.fftCount = m_base.fftCount() > 0 ? m_base.fftCount() : 16384;
    s.pad      = qMax(1, m_base.ionoPad);
    s.overlap  = qMax(1, m_base.ionoOverlap);
    s.snrMode  = m_base.ionoModeSnr;
    s.objW     = m_base.objSizeH;
    s.objH     = m_base.objSizeV;
    s.objLevel = m_base.objLevel;
    return s;
}

void QIGFrame::followLive(const QString &lfsPath)
{
    if (m_live && m_live->path() != lfsPath) {
        /* A newer sounding has begun before the last one's sidecar arrived.
         * That one still reaches the control panel, through addIg. */
        delete m_live;
        m_live = 0;
    }

    bool fresh = false;
    if (!m_live) {
        LiveIonogram *live = new LiveIonogram(lfsPath, liveSettings());
        if (!live->isValid()) {
            /* Header not written yet; the next tick will see it. */
            delete live;
            return;
        }
        if (m_controlIndex < 0 && !m_current->isLive()
                && !m_current->igFileName().isEmpty())
            m_control->load(m_current->igFileName());
        m_live = live;
        fresh = true;
    }

    const bool changed = m_live->update();
    if (m_live->gone()) {
        dropLive();
        return;
    }
    if (changed || fresh) {
        m_current->showLive(*m_live);
        emit liveUpdated(m_live);
    }
}

void QIGFrame::dropLive()
{
    if (!m_live)
        return;
    delete m_live;
    m_live = 0;

    if (m_current->isLive()) {
        if (!m_history.isEmpty())
            m_current->load(m_history.last());
        else
            m_current->clear();
    }
    if (m_controlIndex < 0 && m_history.size() >= 2)
        m_control->load(m_history.at(m_history.size() - 2));
    emit liveEnded();
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
    emit historyChanged();
    if (m_control) m_control->clear();
    if (m_current) m_current->clear();
    if (m_snr)     m_snr->clear();
    if (m_pdp)     m_pdp->clear();
}

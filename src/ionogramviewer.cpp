#include "ionogramviewer.h"

#include "liveionogram.h"
#include "qigframe.h"
#include "qrxionogram.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPen>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

#include <qwt_plot_zoomer.h>
#include <qwt_text.h>

/* Rubber-band zoom, plus a cursor readout in the panel's own units. The
 * delay is also given as a distance, because that is the question being
 * asked of it: is this trace at the range the path says it should be. */
class IgZoomer : public QwtPlotZoomer
{
public:
    explicit IgZoomer(QWidget *canvas) : QwtPlotZoomer(canvas)
    {
        setTrackerMode(QwtPicker::AlwaysOn);
        setRubberBandPen(QPen(Qt::white, 1, Qt::DashLine));
        setTrackerPen(QPen(Qt::black));
    }

protected:
    QwtText trackerTextF(const QPointF &pos) const
    {
        QwtText text(QString::fromUtf8("%1 МГц   %2 мс   %3 км")
                         .arg(pos.x(), 0, 'f', 3)
                         .arg(pos.y(), 0, 'f', 3)
                         .arg(heightKmFromTimeMs(pos.y()), 0, 'f', 0));
        text.setBackgroundBrush(QBrush(QColor(255, 255, 255, 210)));
        return text;
    }
};

static QToolButton *navButton(QWidget *parent, ushort glyph, const char *tip,
                              const QKeySequence &key)
{
    QToolButton *b = new QToolButton(parent);
    b->setText(QChar(glyph));
    b->setToolTip(QString::fromUtf8(tip) + QLatin1String("  (")
                  + key.toString(QKeySequence::NativeText) + QLatin1Char(')'));
    b->setShortcut(key);
    b->setAutoRepeat(true);
    return b;
}

IonogramViewer::IonogramViewer(const QBaseSoundParams &base,
                               const QTxParams &tx,
                               const QRxParams &rx,
                               QIGFrame *frame,
                               const QString &capture,
                               bool follow)
    : QWidget(frame, Qt::Window),
      m_frame(frame),
      m_tx(tx),
      m_plot(0),
      m_zoomer(0),
      m_pos(0),
      m_file(0),
      m_btnBack(0),
      m_btnForward(0),
      m_btnLatest(0),
      m_follow(0)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tx.name);
    resize(1000, 700);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(4);

    QHBoxLayout *bar = new QHBoxLayout();
    bar->setSpacing(4);

    m_btnBack    = navButton(this, 0x25C0, "Предыдущий сеанс", QKeySequence(Qt::Key_Left));
    m_btnForward = navButton(this, 0x25B6, "Следующий сеанс", QKeySequence(Qt::Key_Right));
    m_btnLatest  = navButton(this, 0x21BA, "Последний сеанс", QKeySequence(Qt::Key_End));
    m_btnLatest->setAutoRepeat(false);

    m_pos = new QLabel(this);
    m_pos->setMinimumWidth(70);
    m_pos->setAlignment(Qt::AlignCenter);

    m_follow = new QCheckBox(QString::fromUtf8("Следить за новыми сеансами"), this);

    m_file = new QLabel(this);
    m_file->setStyleSheet(QLatin1String("color: #808080;"));
    m_file->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QToolButton *save = new QToolButton(this);
    save->setText(QString::fromUtf8("Сохранить PNG…"));
    save->setToolButtonStyle(Qt::ToolButtonTextOnly);

    bar->addWidget(m_btnBack);
    bar->addWidget(m_pos);
    bar->addWidget(m_btnForward);
    bar->addWidget(m_btnLatest);
    bar->addSpacing(12);
    bar->addWidget(m_follow);
    bar->addSpacing(12);
    bar->addWidget(m_file, 1);
    bar->addWidget(save);
    outer->addLayout(bar);

    m_plot = new QRxIonogram(base, tx, rx, this);
    m_plot->setMinimumSize(400, 300);
    outer->addWidget(m_plot, 1);

    QLabel *hint = new QLabel(QString::fromUtf8(
        "Левая кнопка: выделить область для увеличения. Правая: исходный масштаб. "
        "← →: соседние сеансы; увеличение при этом сохраняется."),
        this);
    hint->setStyleSheet(QLatin1String("color: #808080;"));
    outer->addWidget(hint);

    m_zoomer = new IgZoomer(m_plot->canvas());

    connect(m_btnBack,    SIGNAL(clicked()), this, SLOT(back()));
    connect(m_btnForward, SIGNAL(clicked()), this, SLOT(forward()));
    connect(m_btnLatest,  SIGNAL(clicked()), this, SLOT(latest()));
    connect(save,         SIGNAL(clicked()), this, SLOT(savePng()));
    connect(m_follow, SIGNAL(toggled(bool)), this, SLOT(followToggled(bool)));
    connect(m_frame,  SIGNAL(historyChanged()), this, SLOT(onHistoryChanged()));
    connect(m_frame,  SIGNAL(liveUpdated(const LiveIonogram *)),
            this,     SLOT(onLiveUpdated(const LiveIonogram *)));
    connect(m_frame,  SIGNAL(liveEnded()), this, SLOT(onLiveEnded()));

    m_follow->setChecked(follow);
    /* Opened on a sounding still being recorded: keep drawing it, rather
     * than computing a one-off picture of the half that exists. */
    const LiveIonogram *live = m_frame->live();
    if (live && live->path() == capture)
        showLive(live);
    else
        showCapture(capture);
}

void IonogramViewer::showLive(const LiveIonogram *live)
{
    if (m_plot->isLive() && m_capture == live->path()) {
        /* Only the new columns; the axes, and so any zoom, stay put. */
        m_plot->showLive(*live);
        return;
    }

    /* The first frame of it sets the axes; keep the zoom as showCapture does. */
    const bool zoomed = m_zoomer->zoomRectIndex() > 0;
    const QRectF region = m_zoomer->zoomRect();

    m_plot->showLive(*live);
    m_capture = live->path();

    m_zoomer->setZoomBase(false);
    if (zoomed)
        m_zoomer->zoom(region);
    else
        m_plot->replot();

    m_file->setText(QFileInfo(m_capture).fileName()
                    + QString::fromUtf8("  — идёт приём"));
    setWindowTitle(m_tx.name + QString::fromUtf8(" — ")
                   + m_plot->startDateTime().toString(
                         QLatin1String("dd.MM.yyyy hh:mm:ss 'UTC'"))
                   + QString::fromUtf8(" — приём"));
    updateNav();
}

void IonogramViewer::onLiveUpdated(const LiveIonogram *live)
{
    if (live && (m_follow->isChecked()
                 || (m_plot->isLive() && m_capture == live->path())))
        showLive(live);
    else
        updateNav();
}

void IonogramViewer::onLiveEnded()
{
    const QStringList &h = m_frame->history();
    if (m_plot->isLive() && !h.isEmpty())
        showCapture(h.last());
    else
        updateNav();
}

void IonogramViewer::showCapture(const QString &capture)
{
    /* Keep the zoomed region when moving to another capture: comparing the
     * same corner of consecutive ionograms is most of what stepping is for. */
    const bool zoomed = m_zoomer->zoomRectIndex() > 0;
    const QRectF region = m_zoomer->zoomRect();

    /* No sidecar write from here: the panels and the sounder own that. */
    if (!m_plot->load(capture, true, false)) {
        m_file->setText(QString::fromUtf8("не удалось открыть ")
                        + QFileInfo(capture).fileName());
        updateNav();
        return;
    }
    m_capture = capture;

    /* load() has rescaled the axes to this capture; that is the new base. */
    m_zoomer->setZoomBase(false);
    if (zoomed)
        m_zoomer->zoom(region);
    else
        m_plot->replot();

    m_file->setText(QFileInfo(capture).fileName());
    setWindowTitle(m_tx.name + QString::fromUtf8(" — ")
                   + m_plot->startDateTime().toString(
                         QLatin1String("dd.MM.yyyy hh:mm:ss 'UTC'")));
    updateNav();
}

void IonogramViewer::updateNav()
{
    const QStringList &h = m_frame->history();
    const int n = h.size();

    if (m_plot->isLive()) {
        /* Past the newest finished capture: only back from here. */
        m_pos->setText(QString::fromUtf8("приём"));
        m_btnBack->setEnabled(n > 0);
        m_btnForward->setEnabled(false);
        m_btnLatest->setEnabled(false);
        return;
    }

    const int at = h.lastIndexOf(m_capture);
    const bool live = m_frame->live() != 0;

    m_pos->setText(at >= 0 ? QString(QLatin1String("%1/%2")).arg(at + 1).arg(n)
                           : QString());
    m_btnBack->setEnabled(at > 0);
    m_btnForward->setEnabled((at >= 0 && at < n - 1) || (live && at == n - 1));
    m_btnLatest->setEnabled((n > 0 && at != n - 1) || live);
}

void IonogramViewer::back()
{
    const QStringList &h = m_frame->history();
    if (m_plot->isLive()) {
        if (h.isEmpty())
            return;
        m_follow->setChecked(false);
        showCapture(h.last());
        return;
    }
    const int at = h.lastIndexOf(m_capture);
    if (at <= 0)
        return;
    /* Looking back is the opposite of following; a new capture arriving
     * should not yank the view away from the one being studied. */
    m_follow->setChecked(false);
    showCapture(h.at(at - 1));
}

void IonogramViewer::forward()
{
    const QStringList &h = m_frame->history();
    const int at = h.lastIndexOf(m_capture);
    if (at >= 0 && at == h.size() - 1 && m_frame->live()) {
        showLive(m_frame->live());
        return;
    }
    if (at < 0 || at >= h.size() - 1)
        return;
    showCapture(h.at(at + 1));
}

void IonogramViewer::latest()
{
    /* The newest is the sounding being recorded, when there is one. */
    if (m_frame->live()) {
        showLive(m_frame->live());
        return;
    }
    const QStringList &h = m_frame->history();
    if (!h.isEmpty())
        showCapture(h.last());
}

void IonogramViewer::followToggled(bool on)
{
    if (!on)
        return;
    if (m_frame->live()) {
        showLive(m_frame->live());
        return;
    }
    const QStringList &h = m_frame->history();
    if (!h.isEmpty() && (h.last() != m_capture || m_plot->isLive()))
        showCapture(h.last());
}

void IonogramViewer::onHistoryChanged()
{
    const QStringList &h = m_frame->history();

    if (m_plot->isLive()) {
        /* The live sounding's own result has arrived: swap it in. Anything
         * else arriving now is older, finishing late, and the live one is
         * still the newest. */
        if (!h.isEmpty() && h.last() == m_capture)
            showCapture(m_capture);
        else
            updateNav();
        return;
    }

    if (m_follow->isChecked() && !h.isEmpty() && h.last() != m_capture
            && !m_frame->live())
        showCapture(h.last());
    else
        updateNav();
}

void IonogramViewer::savePng()
{
    const QString suggested = QDir::home().filePath(
        QFileInfo(m_capture).completeBaseName() + QLatin1String(".png"));
    QString name = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Сохранить ионограмму"), suggested,
        QLatin1String("PNG (*.png)"));
    if (name.isEmpty())
        return;
    if (!name.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
        name += QLatin1String(".png");

    if (!m_plot->grab().save(name, "PNG"))
        m_file->setText(QString::fromUtf8("не удалось сохранить ") + name);
}

#include "stationstack.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

#include <algorithm>

namespace {
const int GRIP_HEIGHT = 7;
}

/* The bar under a panel. Dragging it sets the panel's height. */
class StationGrip : public QWidget
{
public:
    explicit StationGrip(StationStack *stack)
        : QWidget(stack), m_stack(stack), m_startY(0), m_startHeight(0), m_dragging(false)
    {
        setCursor(Qt::SizeVerCursor);
        setToolTip(QString::fromUtf8("Потяните, чтобы изменить высоту панели станции.\n"
                                     "Двойной щелчок — высота по умолчанию."));
    }

protected:
    void paintEvent(QPaintEvent *)
    {
        QPainter p(this);
        p.fillRect(rect(), palette().window());
        /* A short ridge in the middle, so the bar reads as something to grab. */
        p.setPen(palette().mid().color());
        const int cx = width() / 2, cy = height() / 2;
        for (int dx = -30; dx <= 30; dx += 4)
            p.drawPoint(cx + dx, cy);
    }

    void mousePressEvent(QMouseEvent *e)
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        m_startY = e->globalPos().y();
        m_startHeight = m_stack->panelHeight(this);
    }

    void mouseMoveEvent(QMouseEvent *e)
    {
        if (m_dragging)
            m_stack->setPanelHeight(this, std::max(1, m_startHeight + e->globalPos().y() - m_startY));
    }

    void mouseReleaseEvent(QMouseEvent *e)
    {
        if (!m_dragging || e->button() != Qt::LeftButton)
            return;
        m_dragging = false;
        m_stack->released(this);
    }

    void mouseDoubleClickEvent(QMouseEvent *e)
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = false;
        m_stack->setPanelHeight(this, 0);
        m_stack->released(this);
    }

private:
    StationStack *m_stack;
    int  m_startY;
    int  m_startHeight;
    bool m_dragging;
};

StationStack::StationStack(QWidget *parent)
    : QWidget(parent), m_viewport(0), m_inLayout(false)
{
}

StationStack::~StationStack()
{
    /* The panels are our children and go with us; by then this object is
     * only a QWidget, and must not hear about it. */
    for (int i = 0; i < m_items.size(); ++i)
        disconnect(m_items.at(i).panel, 0, this, 0);
}

void StationStack::addStation(QWidget *panel, const QString &name, int height)
{
    Item item;
    item.panel = panel;
    item.grip = new StationGrip(this);
    item.name = name;
    item.height = std::max(0, height);
    panel->setParent(this);
    connect(panel, SIGNAL(destroyed(QObject*)), this, SLOT(panelDestroyed(QObject*)));
    m_items << item;
    panel->show();
    item.grip->show();
    relayout();
}

void StationStack::followViewport(QWidget *viewport)
{
    if (m_viewport)
        m_viewport->removeEventFilter(this);
    m_viewport = viewport;
    if (m_viewport)
        m_viewport->installEventFilter(this);
    relayout();
}

void StationStack::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool StationStack::eventFilter(QObject *watched, QEvent *event)
{
    /* The viewport can change height while we stay at our minimum, which
     * sends us nothing: the shared panels still have to be worked out again. */
    if (watched == m_viewport && event->type() == QEvent::Resize)
        relayout();
    return QWidget::eventFilter(watched, event);
}

void StationStack::panelDestroyed(QObject *panel)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (static_cast<QObject *>(m_items.at(i).panel) == panel) {
            delete m_items.at(i).grip;
            m_items.removeAt(i);
            break;
        }
    }
    relayout();
}

int StationStack::minimumOf(const Item &item) const
{
    return std::max(item.panel->minimumSizeHint().height(), item.panel->minimumHeight());
}

int StationStack::indexOf(const StationGrip *grip) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).grip == grip)
            return i;
    return -1;
}

int StationStack::panelHeight(const StationGrip *grip) const
{
    const int i = indexOf(grip);
    return i < 0 ? 0 : m_items.at(i).panel->height();
}

void StationStack::setPanelHeight(const StationGrip *grip, int height)
{
    const int i = indexOf(grip);
    if (i < 0)
        return;
    m_items[i].height = height > 0 ? std::max(height, minimumOf(m_items.at(i))) : 0;
    relayout();
}

void StationStack::released(const StationGrip *grip)
{
    const int i = indexOf(grip);
    if (i >= 0)
        emit heightChanged(m_items.at(i).name, m_items.at(i).height);
}

void StationStack::relayout()
{
    if (m_inLayout)
        return;
    m_inLayout = true;

    const int visible = m_viewport ? m_viewport->height() : height();
    int fixedSum = 0, shared = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).height > 0)
            fixedSum += std::max(m_items.at(i).height, minimumOf(m_items.at(i)));
        else
            ++shared;
    }
    const int spare = visible - fixedSum - GRIP_HEIGHT * m_items.size();
    const int share = shared > 0 ? spare / shared : 0;

    int y = 0;
    const int w = width();
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items.at(i);
        const int h = std::max(it.height > 0 ? it.height : share, minimumOf(it));
        it.panel->setGeometry(0, y, w, h);
        y += h;
        it.grip->setGeometry(0, y, w, GRIP_HEIGHT);
        y += GRIP_HEIGHT;
    }

    /* The scroll area sizes us to the viewport, but never below this. */
    setMinimumHeight(y);
    const int want = std::max(y, visible);
    if (height() != want)
        resize(w, want);

    m_inLayout = false;
}

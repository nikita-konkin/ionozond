#ifndef STATIONSTACK_H
#define STATIONSTACK_H

#include <QList>
#include <QString>
#include <QWidget>

class StationGrip;

/*
 * The station panels, one above another, each with a grip along its bottom
 * edge that sets its height. Not in the original, which had a splitter: there
 * making one panel taller could only make its neighbour shorter, and with
 * three stations every panel sat at its minimum and none could be resized.
 *
 * A panel nobody has resized shares the visible height with the others, as
 * the splitter's did. One that has been resized keeps its height, and the
 * scroll area around the stack takes whatever no longer fits. A double click
 * on the grip hands the panel back to sharing.
 */
class StationStack : public QWidget
{
    Q_OBJECT

public:
    explicit StationStack(QWidget *parent = 0);
    ~StationStack();

    /* The stack takes the panel over. `height` 0 = share the visible height.
     * Deleting the panel elsewhere takes it out of the stack. */
    void addStation(QWidget *panel, const QString &name, int height = 0);

    /* The scroll area's viewport: its height is what shared panels divide. */
    void followViewport(QWidget *viewport);

signals:
    /* A grip let go of: the panel's height, or 0 when reset. */
    void heightChanged(const QString &name, int height);

protected:
    void resizeEvent(QResizeEvent *event);
    bool eventFilter(QObject *watched, QEvent *event);

private slots:
    void panelDestroyed(QObject *panel);

private:
    friend class StationGrip;

    struct Item {
        QWidget     *panel;
        StationGrip *grip;
        QString      name;
        int          height;   // 0 = share
    };

    void relayout();
    int  minimumOf(const Item &item) const;
    int  indexOf(const StationGrip *grip) const;
    /* From the grips. */
    int  panelHeight(const StationGrip *grip) const;
    void setPanelHeight(const StationGrip *grip, int height);
    void released(const StationGrip *grip);

    QList<Item> m_items;
    QWidget    *m_viewport;
    bool        m_inLayout;
};

#endif /* STATIONSTACK_H */

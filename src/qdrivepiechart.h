#ifndef QDRIVEPIECHART_H
#define QDRIVEPIECHART_H

#include <QFont>
#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

/*
 * QIgDrivePieChartLabel @0x4753?? -- one legend entry beside the pie: a
 * colour swatch, a name, and "%1GB(%2%)" (@0x494603).
 */
class QIgDrivePieChartLabel : public QObject
{
    Q_OBJECT
public:
    QIgDrivePieChartLabel(const QString &name, QObject *parent = 0);

    void    setName(const QString &name) { m_name = name; }
    QString getName() const              { return m_name; }

    void setFont(const QFont &font) { m_font = font; }
    QFont getFont() const           { return m_font; }

    void   setPos(const int &x, const int &y) { m_pos = QPoint(x, y); }
    void   setPos(const QPoint &pos)          { m_pos = pos; }
    QPoint getPos() const                     { return m_pos; }

    void    setInfo(const float &gigabytes, const unsigned int &percent);
    QString getInfoString() const  { return m_info; }
    QString getShortString() const { return m_short; }

    int getHeight() const;
    int getWidth() const;

private:
    QString m_name;
    QString m_info;
    QString m_short;
    QFont   m_font;
    QPoint  m_pos;
};

/*
 * QDrivePieChart -- the "Диск" panel: a pie of used vs free space on the
 * ionogram directory's filesystem, with a two-line legend.
 *
 * moc: signal driveSizesChanged(), slots onDriveSizesChanged(), onTimerUpdated()
 */
class QDrivePieChart : public QWidget
{
    Q_OBJECT

public:
    explicit QDrivePieChart(QWidget *parent = 0);

    void setPath(const QString &path);

signals:
    void driveSizesChanged();

public slots:
    void onDriveSizesChanged();
    void onTimerUpdated();

protected:
    virtual void paintEvent(QPaintEvent *event);

private:
    QString m_path;
    double  m_usedGb;
    double  m_freeGb;
    QTimer *m_timer;

    QIgDrivePieChartLabel *m_usageLabel;
    QIgDrivePieChartLabel *m_freeLabel;
};

#endif /* QDRIVEPIECHART_H */

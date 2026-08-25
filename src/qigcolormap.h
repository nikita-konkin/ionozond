#ifndef QIGCOLORMAP_H
#define QIGCOLORMAP_H

#include <QColor>
#include <QVector>

#include <qwt_color_map.h>

/*
 * A single gradient stop: a position in [0,1] and its colour.
 * QColorLevel is 24 bytes in the original (double + QColor).
 */
class QColorLevel
{
public:
    QColorLevel() : m_level(0.0) {}
    QColorLevel(double level, const QColor &color)
        : m_level(level), m_color(color) {}

    double level() const { return m_level; }
    QColor color() const { return m_color; }

private:
    double m_level;
    QColor m_color;
};

/*
 * QIgColorMap @0x4754e0
 *
 * A QwtLinearColorMap built from a list of stops:
 *   - warns if fewer than 3 stops ("number of color levels less than 3")
 *   - setColorInterval(levels.first().color(), levels.last().color())
 *   - addColorStop(level, colour) for every stop from the second onwards
 */
class QIgColorMap : public QwtLinearColorMap
{
public:
    explicit QIgColorMap(const QVector<QColorLevel> &levels,
                         const QwtLinearColorMap::Mode &mode = QwtLinearColorMap::ScaledColors);
};

/* The recovered maps (qigcolormap_tables.cpp, generated). */
const QVector<QColorLevel> &PDP_COLORS();
const QVector<QColorLevel> &BLUE_BASE_COLORS();
const QVector<QColorLevel> &WHITE_BASE_COLORS();
const QVector<QColorLevel> &IG2_COLORS();
const QVector<QColorLevel> &IG2_MOD_COLORS();
const QVector<QColorLevel> &GRAY_COLORS();
const QVector<QColorLevel> &RAINBOW_COLORS();
const QVector<QColorLevel> &RAINBOW_WHITEBASE_COLORS();
const QVector<QColorLevel> &IG2_BASE_COLORS();
const QVector<QColorLevel> &IG2_MOD_BASE_COLORS();

/* Map selected by the ig_colormap_index / colormap_index settings. */
const QVector<QColorLevel> &colorLevelsForIndex(int index);

#endif /* QIGCOLORMAP_H */

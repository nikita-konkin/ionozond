#include "qigcolormap.h"

#include <QDebug>

QIgColorMap::QIgColorMap(const QVector<QColorLevel> &levels,
                         const QwtLinearColorMap::Mode &mode)
    : QwtLinearColorMap()
{
    if (levels.size() < 3) {
        qWarning("QIgColorMap::QIgColorMap: number of color levels less than 3");
        return;
    }

    setMode(mode);
    setColorInterval(levels.first().color(), levels.last().color());

    /* The original starts at the second element and runs to the end. */
    for (int i = 1; i < levels.size(); ++i)
        addColorStop(levels.at(i).level(), levels.at(i).color());
}

const QVector<QColorLevel> &colorLevelsForIndex(int index)
{
    /*
     * IG_COLORMAP_LIST, read straight off the appends in the static
     * initialiser @0x41a075 (unlike the QStringList combos, this vector is
     * appended in construction order, so no reversal applies).
     *
     * PDP_COLORS is deliberately not in this list -- it belongs to the
     * power-delay-profile widget rather than the colour-map selector.
     *
     * The shipped config uses ig_colormap_index=1 (WHITE_BASE_COLORS) for the
     * ionogram, which gives the white background of the manual's plots, and
     * colormap_index=8 (IG2_MOD_BASE_COLORS) for the variation plots, whose
     * colour bar runs magenta -> blue -> green -> yellow -> red.
     */
    switch (index) {
    case 0:  return BLUE_BASE_COLORS();
    case 1:  return WHITE_BASE_COLORS();
    case 2:  return IG2_COLORS();
    case 3:  return IG2_MOD_COLORS();
    case 4:  return GRAY_COLORS();
    case 5:  return RAINBOW_COLORS();
    case 6:  return RAINBOW_WHITEBASE_COLORS();
    case 7:  return IG2_BASE_COLORS();
    case 8:  return IG2_MOD_BASE_COLORS();
    case 9:  return JET_COLORS();
    default: return WHITE_BASE_COLORS();
    }
}

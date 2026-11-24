#ifndef RASTERINTERVALS_H
#define RASTERINTERVALS_H

#include <qwt_global.h>
#include <qwt_interval.h>
#include <qwt_raster_data.h>

/*
 * Raster data that keeps its three axis intervals itself.
 *
 * Qwt 6.1 held the intervals inside QwtRasterData behind setInterval(); Qwt
 * 6.3 removed setInterval() and made interval() pure virtual. Every raster
 * here derives from this instead, so the same calls build against Ubuntu
 * 22.04's Qwt 6.1.4 and against 6.3 alike.
 */
class RasterIntervals : public QwtRasterData
{
#if QWT_VERSION >= 0x060300
public:
    void setInterval(Qt::Axis axis, const QwtInterval &interval)
    { m_intervals[static_cast<int>(axis)] = interval; }
    QwtInterval interval(Qt::Axis axis) const override
    { return m_intervals[static_cast<int>(axis)]; }

private:
    QwtInterval m_intervals[3];
#endif
};

#endif /* RASTERINTERVALS_H */

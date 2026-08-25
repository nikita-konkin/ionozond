#ifndef RASTERDATA_H
#define RASTERDATA_H

#include <QRectF>

#include <qwt_raster_data.h>

/*
 * RasterData @0x472170
 *
 * Feeds a QwtPlotSpectrogram from the ionogram array.
 *
 *   xIndex(x)  = (int)((x - area.left()) / xStep)     @0x472380
 *   yIndex(y)  = (int)((y - area.top())  / yStep)     @0x4723a0
 *   value(x,y) = data[xIndex(x)][yIndex(y)]           @0x4723c0
 *
 * x selects the spectrum (the frequency axis), y the point within it (the
 * delay axis). The original does no bounds checking at all; this version
 * clamps, which cannot change the rendered result for in-range queries but
 * avoids a crash if Qwt probes slightly outside the interval.
 */
class RasterData : public QwtRasterData
{
public:
    RasterData(float **data, const int &specCount, const int &specPointCount,
               const QRectF &area, const float &powerMax);
    virtual ~RasterData();

    void setData(float **data);
    void setPowerMax(const float &powerMax);

    int xIndex(const double &x) const;
    int yIndex(const double &y) const;

    virtual double value(double x, double y) const;

private:
    float **m_data;
    int     m_specCount;
    int     m_specPointCount;
    QRectF  m_area;
    float   m_xStep;
    float   m_yStep;
    float   m_powerMax;
};

#endif /* RASTERDATA_H */

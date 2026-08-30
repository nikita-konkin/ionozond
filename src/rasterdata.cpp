#include "rasterdata.h"

#include <algorithm>

RasterData::RasterData(float **data, const int &specCount, const int &specPointCount,
                       const QRectF &area, const float &powerMax)
    : QwtRasterData(),
      m_data(data),
      m_specCount(specCount),
      m_specPointCount(specPointCount),
      m_area(area),
      m_xStep(0.0f),
      m_yStep(0.0f),
      m_powerMax(powerMax),
      m_powerMin(0.0f)
{
    if (m_specCount > 0)
        m_xStep = (float)(m_area.width() / m_specCount);
    if (m_specPointCount > 0)
        m_yStep = (float)(m_area.height() / m_specPointCount);

    setInterval(Qt::XAxis, QwtInterval(m_area.left(), m_area.right()));
    setInterval(Qt::YAxis, QwtInterval(m_area.top(), m_area.bottom()));
    setInterval(Qt::ZAxis, QwtInterval(m_powerMin, m_powerMax));
}

RasterData::~RasterData()
{
}

void RasterData::setData(float **data)
{
    m_data = data;
}

void RasterData::setPowerMin(const float &powerMin)
{
    m_powerMin = powerMin;
    setInterval(Qt::ZAxis, QwtInterval(m_powerMin, m_powerMax));
}

void RasterData::setPowerMax(const float &powerMax)
{
    m_powerMax = powerMax;
    setInterval(Qt::ZAxis, QwtInterval(m_powerMin, m_powerMax));
}

int RasterData::xIndex(const double &x) const
{
    if (m_xStep == 0.0f)
        return 0;
    return (int)((x - m_area.left()) / (double)m_xStep);
}

int RasterData::yIndex(const double &y) const
{
    if (m_yStep == 0.0f)
        return 0;
    return (int)((y - m_area.top()) / (double)m_yStep);
}

double RasterData::value(double x, double y) const
{
    if (!m_data || m_specCount <= 0 || m_specPointCount <= 0)
        return 0.0;

    /* The original indexes unchecked; clamping only guards the edges. */
    const int ix = std::min(std::max(xIndex(x), 0), m_specCount - 1);
    const int iy = std::min(std::max(yIndex(y), 0), m_specPointCount - 1);

    const float *spec = m_data[ix];
    return spec ? (double)spec[iy] : 0.0;
}

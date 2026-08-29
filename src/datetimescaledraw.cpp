#include "datetimescaledraw.h"

#include <QDateTime>

DateTimeScaleDraw::DateTimeScaleDraw(const QString &format)
    : QwtScaleDraw(), m_format(format)
{
}

void DateTimeScaleDraw::setFormat(const QString &format)
{
    m_format = format;
}

QString DateTimeScaleDraw::format() const
{
    return m_format;
}

QwtText DateTimeScaleDraw::label(double value) const
{
    const QDateTime dt =
        QDateTime::fromMSecsSinceEpoch((qint64)(value * 1000.0), Qt::UTC);

    /*
     * A fixed "dd MMM" made every tick read the same date, which is what the
     * axis looked like on a station recording every five minutes: an hour of
     * soundings labelled "29 Aug, 29 Aug, 29 Aug". Choose the unit from the
     * span actually on screen instead, so the axis says something whether it
     * holds ten minutes or ten days.
     */
    if (m_format.isEmpty()) {
        const double span = scaleDiv().upperBound() - scaleDiv().lowerBound();
        if (span <= 0.0)
            return QwtText(dt.toString(QLatin1String("hh:mm")));
        if (span < 2.0 * 3600.0)                 /* under two hours */
            return QwtText(dt.toString(QLatin1String("hh:mm:ss")));
        if (span < 36.0 * 3600.0)                /* within a day or so */
            return QwtText(dt.toString(QLatin1String("hh:mm")));
        if (span < 30.0 * 86400.0)
            return QwtText(dt.toString(QLatin1String("dd MMM hh:mm")));
        return QwtText(dt.toString(QLatin1String("dd MMM")));
    }
    return QwtText(dt.toString(m_format));
}

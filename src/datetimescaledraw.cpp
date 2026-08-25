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
    return QwtText(dt.toString(m_format));
}

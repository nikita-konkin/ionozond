#ifndef DATETIMESCALEDRAW_H
#define DATETIMESCALEDRAW_H

#include <QString>

#include <qwt_scale_draw.h>
#include <qwt_text.h>

/*
 * DateTimeScaleDraw @0x4?????  -- formats a time axis whose values are
 * seconds since the epoch.
 *
 * Format strings seen in the original's .rodata: "dd MMM hh:mm:ss"
 * (@0x4942f0), "dd MMM" (@0x4953d9), "MM/dd" (@0x494cee), "hh:mm"
 * (@0x494cf4). All times are rendered in UTC, as the panels are labelled.
 */
class DateTimeScaleDraw : public QwtScaleDraw
{
public:
    /* An empty format means "choose the unit from the span on screen" --
     * seconds for minutes of data, times within a day, dates beyond that. */
    explicit DateTimeScaleDraw(const QString &format = QString());

    void    setFormat(const QString &format);
    QString format() const;

    virtual QwtText label(double value) const;

private:
    QString m_format;
};

#endif /* DATETIMESCALEDRAW_H */

#include "schedule.h"

SessionTimes calcNextSessionTimes(const QDateTime &now,
                                  quint32 rep,
                                  quint32 chirpt,
                                  quint32 dur)
{
    SessionTimes times;
    if (rep == 0)
        return times;

    const qint64 t = now.toMSecsSinceEpoch() / 1000;

    /* The chirp instant of the slot containing `t`. */
    qint64 start = (t / rep) * (qint64)rep + (qint64)chirpt;

    /* The original always targets the next chirp still to come: at 22:09:01,
     * with a session running since 22:05:10, it displayed 22:10:10 rather than
     * the session in progress. (Behaviour exactly at the chirp instant is not
     * observable from the GUI; strictly-after is assumed.) */
    if (start <= t)
        start += rep;

    times.start = QDateTime::fromMSecsSinceEpoch(start * 1000, Qt::UTC);
    times.stop = QDateTime::fromMSecsSinceEpoch((start + (qint64)dur) * 1000, Qt::UTC);
    return times;
}

QString calcIgFileName(const QString &dataDir,
                       const QString &txName,
                       const QDateTime &sessionStart)
{
    const QDateTime utc = sessionStart.toUTC();
    return QString(QLatin1String("%1%2/%3_%4_%5.lfs"))
        .arg(dataDir)
        .arg(utc.toString(QLatin1String("yyyy.MM.dd")))
        .arg(txName)
        .arg(utc.toString(QLatin1String("yyyyMMdd")))
        .arg(utc.toString(QLatin1String("hhmmss")));
}

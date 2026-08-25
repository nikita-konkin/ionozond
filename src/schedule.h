#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <QDateTime>
#include <QString>

/*
 * Session timing and ionogram file naming.
 *
 * Reconstructed from QActiveScheduleItem::CalcNextSessionTimes(QDateTime const&)
 * @0x465420 and QActiveScheduleItem::CalcIGFileName() @0x465960. Kept separate
 * from the QObject so the arithmetic can be tested without a GUI.
 */

struct SessionTimes {
    QDateTime start;
    QDateTime stop;
};

/*
 * A transmitter chirps every `rep` seconds, at the instant where
 * (unix time) % rep == chirpt, and each sounding lasts `dur` seconds.
 *
 * Returns the first session that has not yet finished at `now`.
 * Verified against the original: cyprus1 (rep=300, chirpt=10, dur=250) at
 * 22:09:01 UTC gives 22:10:10 -> 22:14:20, which is what the running binary
 * displays in its session panel.
 */
SessionTimes calcNextSessionTimes(const QDateTime &now,
                                  quint32 rep,
                                  quint32 chirpt,
                                  quint32 dur);

/*
 * Path of the capture for a session:
 *     <dataDir><yyyy.MM.dd>/<txName>_<yyyyMMdd>_<hhmmss>.lfs
 *
 * from the format string "%1%2/%3_%4_%5.lfs" @0x493ce7. dataDir is expected to
 * carry its own trailing separator, as it does in config.ini. All formatting is
 * done in UTC.
 */
QString calcIgFileName(const QString &dataDir,
                       const QString &txName,
                       const QDateTime &sessionStart);

#endif /* SCHEDULE_H */

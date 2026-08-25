/*
 * Verification 4: session timing and .lfs file naming.
 *
 * The timing case is taken from the ORIGINAL binary: driven under Xvfb with
 * cyprus1 (rep=300, chirpt=10, dur=250), at 22:09:01 UTC its session panel
 * showed 22:10:10 -> 22:14:20 (tests/out/oracle_after_click.png).
 *
 * The file naming cases are taken from the real captures in F:\MyData\ND\lfs
 * and their directory layout.
 */
#include "../src/schedule.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QTime>

#include <cstdio>

static int failures = 0;

static void expectStr(const char *what, const QString &got, const QString &want)
{
    const bool ok = (got == want);
    std::printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        std::printf("      want: %s\n", want.toUtf8().constData());
        std::printf("      got : %s\n", got.toUtf8().constData());
        ++failures;
    }
}

static QDateTime utc(int y, int m, int d, int hh, int mm, int ss)
{
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss), Qt::UTC);
}

static void test_session_times_vs_original()
{
    std::printf("calcNextSessionTimes vs original binary:\n");

    /* cyprus1: rep=300, chirpt=10, dur=250 */
    SessionTimes t = calcNextSessionTimes(utc(2026, 8, 24, 22, 9, 1), 300, 10, 250);
    expectStr("cyprus1 @22:09:01 -> start",
              t.start.toString(QLatin1String("hh:mm:ss")), QLatin1String("22:10:10"));
    expectStr("cyprus1 @22:09:01 -> stop",
              t.stop.toString(QLatin1String("hh:mm:ss")), QLatin1String("22:14:20"));

    /* Even with a session in progress the next chirp is what gets scheduled --
     * this is what the original did at 22:09:01, mid-way through the session
     * that began at 22:05:10. */
    t = calcNextSessionTimes(utc(2026, 8, 24, 22, 12, 0), 300, 10, 250);
    expectStr("mid-session -> still the next chirp",
              t.start.toString(QLatin1String("hh:mm:ss")), QLatin1String("22:15:10"));

    t = calcNextSessionTimes(utc(2026, 8, 24, 22, 14, 20), 300, 10, 250);
    expectStr("after end -> next slot",
              t.start.toString(QLatin1String("hh:mm:ss")), QLatin1String("22:15:10"));

    /* The chirp instant always satisfies (epoch % rep) == chirpt. */
    t = calcNextSessionTimes(utc(2026, 8, 24, 22, 9, 1), 300, 10, 250);
    const qint64 epoch = t.start.toMSecsSinceEpoch() / 1000;
    const bool aligned = (epoch % 300) == 10;
    std::printf("  %-34s %s\n", "start aligned to (epoch%rep)==chirpt",
                aligned ? "ok" : "FAIL");
    if (!aligned) ++failures;
}

static void test_ig_file_name()
{
    std::printf("calcIgFileName vs real captures:\n");

    expectStr("cyprus1 2019-10-23 07:15:10",
              calcIgFileName(QLatin1String("/data/"), QLatin1String("cyprus1"),
                             utc(2019, 10, 23, 7, 15, 10)),
              QLatin1String("/data/2019.10.23/cyprus1_20191023_071510.lfs"));

    expectStr("cyprus1 2019-10-23 09:00:10",
              calcIgFileName(QLatin1String("/data/"), QLatin1String("cyprus1"),
                             utc(2019, 10, 23, 9, 0, 10)),
              QLatin1String("/data/2019.10.23/cyprus1_20191023_090010.lfs"));

    /* Directory layout matches the archive on disk (e.g. 2026.02.04). */
    expectStr("cyprus2 2021-03-03 11:00:15",
              calcIgFileName(QLatin1String("/media/DATA3/ionozond_data/"),
                             QLatin1String("cyprus2"), utc(2021, 3, 3, 11, 0, 15)),
              QLatin1String("/media/DATA3/ionozond_data/2021.03.03/cyprus2_20210303_110015.lfs"));

    /* 24-hour clock: "hh" must not fold 13:00 to 01:00. */
    expectStr("afternoon uses 24h clock",
              calcIgFileName(QLatin1String("/d/"), QLatin1String("norilsk"),
                             utc(2020, 1, 2, 13, 4, 5)),
              QLatin1String("/d/2020.01.02/norilsk_20200102_130405.lfs"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_session_times_vs_original();
    test_ig_file_name();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

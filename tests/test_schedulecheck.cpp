/*
 * The schedule checks: collisions found by replaying the sounder's own choice
 * of what to record, and the values chirp_config.py or the sounder cannot
 * take. Starts from the station's schedule as it stood on 2026-10-09.
 */
#include "../src/schedulecheck.h"

#include <QCoreApplication>

#include <cstdio>

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

static QString u8(const char *s) { return QString::fromUtf8(s); }

static ScheduleEntry station(const char *name, bool active, const char *rep,
                             const char *chirpt, const char *rate, const char *dur,
                             const char *cf, const char *lat, const char *lon, int fft = 0)
{
    ScheduleEntry e;
    e.name = u8(name);
    e.active = active;
    e.rep = QLatin1String(rep);
    e.chirpt = QLatin1String(chirpt);
    e.rate = QLatin1String(rate);
    e.dur = QLatin1String(dur);
    e.cf = QLatin1String(cf);
    e.lat = QLatin1String(lat);
    e.lon = QLatin1String(lon);
    e.fft = fft;
    return e;
}

static ScheduleEntry receiver()
{
    ScheduleEntry e = station("yoshkar-ola", false, "60", "0", "50", "260", "13500",
                              "56.38", "47.53");
    e.rx = true;
    return e;
}

static CheckConfig config()
{
    CheckConfig c;
    c.checkFiles = false;
    return c;
}

/* An issue of `level` for `who` whose text contains `part`. */
static bool has(const QList<CheckIssue> &issues, CheckIssue::Level level,
                const char *who, const char *part)
{
    for (int i = 0; i < issues.size(); ++i)
        if (issues.at(i).level == level && issues.at(i).station == u8(who)
                && issues.at(i).text.contains(u8(part)))
            return true;
    return false;
}

static void dump(const QList<CheckIssue> &issues)
{
    for (int i = 0; i < issues.size(); ++i)
        std::printf("    %s\n", issueLine(issues.at(i)).toUtf8().constData());
}

static QList<CheckIssue> run(const QList<ScheduleEntry> &entries,
                             const CheckConfig &cfg = config())
{
    const QList<CheckIssue> issues = checkSchedule(entries, cfg);
    dump(issues);
    return issues;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::printf("--- the station's schedule\n");
    {
        QList<ScheduleEntry> s;
        s << station("Magadan", true, "600", "8", "500", "52", "16500", "60", "150.7", 4096)
          << station("NIC1_235", false, "300", "0", "100", "235", "20000", "35", "33")
          << station("Norilsk", true, "600", "188", "500", "52", "16500", "69.2", "88", 4096)
          << station("Sodankyla", false, "300", "121.0015", "500.0084", "50", "16000",
                     "67.37", "26.63")
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(countIssues(is, CheckIssue::Error) == 0, "no errors");
        check(has(is, CheckIssue::Warning, "Magadan", "шире полосы")
              && has(is, CheckIssue::Warning, "Norilsk", "Довольно 50 с"),
              "52 s at 500 kHz/s is 2 s past the band");
        check(has(is, CheckIssue::Info, "", "не пересекаются"), "Magadan and Norilsk do not collide");
        check(!has(is, CheckIssue::Warning, "Magadan", "теряет")
              && !has(is, CheckIssue::Warning, "Norilsk", "теряет"), "nothing lost");
        check(!has(is, CheckIssue::Info, "Magadan", "ячейка"), "4096 at 500 kHz/s is the right cell");

        /* Sodankyla's odd rate is past the band edge by 0.8 ms: not news. */
        s[3].active = true;
        s[0].active = s[2].active = false;
        check(!has(run(s), CheckIssue::Warning, "Sodankyla", "шире полосы"),
              "50 s at 500.0084 kHz/s is not flagged");
    }

    std::printf("--- one station inside another's capture\n");
    {
        QList<ScheduleEntry> s;
        s << station("A", true, "300", "0", "100", "250", "12500", "35", "33")
          << station("B", true, "300", "100", "100", "50", "12500", "35", "34")
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Warning, "B", "теряет 288 из 288"), "B loses every session to A");
        check(has(is, CheckIssue::Warning, "B", "у A до 80 с"), "and is told how short A must be");
        check(!has(is, CheckIssue::Warning, "A", "теряет"), "A loses none");
    }

    std::printf("--- a third station in a five-minute schedule (2026-10-10)\n");
    {
        QList<ScheduleEntry> s;
        s << station("Magadan", true, "300", "0", "500", "52", "16500", "60", "150.7", 4096)
          << station("NIC_900", true, "600", "315", "100", "240", "20000", "35.18", "33.38")
          << station("Norilsk", true, "300", "180", "500", "52", "16500", "69.2", "88", 4096)
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Warning, "NIC_900", "теряет 144 из 144"),
              "NIC_900 at 5:15 falls inside Magadan's 5:00 capture");
        check(has(is, CheckIssue::Warning, "NIC_900", "через сеанс (период 600 с, chirptime 0)"),
              "advice: record Magadan every other session");
        check(!has(is, CheckIssue::Warning, "NIC_900", "до 6 с"),
              "not: cut a 26 MHz sweep to 3 MHz");
    }

    std::printf("--- the same second\n");
    {
        QList<ScheduleEntry> s;
        s << station("A", true, "600", "10", "100", "100", "12500", "35", "33")
          << station("B", true, "600", "10", "100", "100", "12500", "35", "34")
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Warning, "B", "теряет 144 из 144")
              && has(is, CheckIssue::Warning, "B", "одновременно"),
              "a tie goes to the first listed; the other is told");
    }

    std::printf("--- room for the capture but not for the sidecar\n");
    {
        QList<ScheduleEntry> s;
        s << station("A", true, "600", "0", "100", "250", "12500", "35", "33")
          << station("B", true, "600", "260", "100", "50", "12500", "35", "34")
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(!has(is, CheckIssue::Warning, "B", "теряет"), "10 s apart: not certain");
        check(has(is, CheckIssue::Warning, "B", "может терять до 144"), "but at risk");
    }

    std::printf("--- a session longer than its period\n");
    {
        QList<ScheduleEntry> s;
        s << station("A", true, "60", "0", "100", "60", "12500", "35", "33") << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Warning, "A", "записывается 720 из 1440"), "every other one");
    }

    std::printf("--- values chirp_config.py cannot carry\n");
    {
        QList<ScheduleEntry> s;
        s << station("Норильск", true, "600", "188", "500", "50", "16500", "69.2", "88", 4096)
          << station("C", true, "0300", "0", "100", "250", "12500", "56,38", "33")
          << station("D", true, "0", "0", "100", "250", "12500", "35", "33")
          << station("E'", true, "300", "0", "100", "250", "12500", "35", "33")
          << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Error, "Норильск", "Latin-1"), "a Cyrillic name");
        check(has(is, CheckIssue::Error, "C", "ведущим нулём"), "0300");
        check(has(is, CheckIssue::Error, "C", "запятой"), "56,38");
        check(has(is, CheckIssue::Error, "D", "зависнет"), "rep 0");
        check(has(is, CheckIssue::Error, "E'", "кавычка"), "a quote in the name");
    }

    std::printf("--- band, window, FFT, timing\n");
    {
        QList<ScheduleEntry> s;
        s << station("Fast", true, "600", "0", "1000", "25", "12500", "59.56", "150.80", 2048)
          << station("Big", true, "600", "200", "500", "50", "16000", "67.37", "26.63", 16384)
          << station("Odd", true, "7000", "7100", "100", "250", "12500", "35", "33")
          << station("Tiny", true, "600", "400", "100", "30", "12500", "35", "33", 65536)
          << station("Tinier", true, "600", "500", "100", "10", "12500", "35", "33", 65536)
          << receiver() << receiver();
        const QList<CheckIssue> is = run(s);
        check(has(is, CheckIssue::Error, "Fast", "окно задержки"), "1000 kHz/s cannot reach Magadan's delay");
        check(has(is, CheckIssue::Info, "Big", "БПФ 4096"), "16384 at 500 kHz/s: advise 4096");
        check(has(is, CheckIssue::Warning, "Odd", "chirptime 7100"), "chirptime past the period");
        check(has(is, CheckIssue::Warning, "Odd", "не делятся"), "a period that does not divide a day");
        check(has(is, CheckIssue::Warning, "Tiny", "спектров"), "65536 points in 30 s: coarse");
        check(has(is, CheckIssue::Error, "Tinier", "удалит всё"), "65536 points in 10 s: nothing left");
        check(has(is, CheckIssue::Error, "yoshkar-ola", "одним названием"), "two columns, one name");
    }

    std::printf("--- general settings\n");
    {
        QList<ScheduleEntry> s;
        s << station("A", true, "300", "0", "100", "250", "12500", "35", "33");
        CheckConfig c = config();
        c.objLevel = 28;
        const QList<CheckIssue> is = run(s, c);
        check(has(is, CheckIssue::Error, "", "удалено всё"), "obj_level above 9 x 3");
        check(has(is, CheckIssue::Warning, "", "приёмная станция не выбрана"), "no receiver");
        c.objLevel = 27;
        check(!has(checkSchedule(s, c), CheckIssue::Error, "", "удалено всё"), "27 is allowed");
        c.sampleRateIndex = 2;   // 12500
        check(has(checkSchedule(s, c), CheckIssue::Warning, "", "по-разному"),
              "sample_rate and its index disagree");
    }

    std::printf("--- the sounder's choice itself\n");
    {
        SimStation a;
        a.rep = 300; a.chirpt = 1; a.dur = 0; a.post = 0;
        /* From midnight, chirptime 1 s is less than 3 s away: the first
         * session taken is the next one. */
        const QVector<SimCapture> c = simulateSounder(QVector<SimStation>() << a, 0, 1000);
        check(c.size() == 3 && c.at(0).start == 301 && c.at(2).start == 901,
              "the 3 s lead skips a chirp too close");
    }

    std::printf(failures ? "FAILED (%d failures)\n" : "PASSED (0 failures)\n", failures);
    return failures ? 1 : 0;
}

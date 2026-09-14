/*
 * Verification 3: frmMain::CreateConfigFile() must reproduce, byte for byte,
 * what the ORIGINAL binary writes from the same settings.
 *
 * The golden files in tests/golden/ were captured from the original running
 * headless -- see tests/golden/README.md. Nothing here is hand-written.
 *
 *   test_configwriter <golden_dir>
 */
#include "../src/configwriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

static QString readAll(const QString &path, bool *ok)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *ok = false; return QString(); }
    QString s = QString::fromUtf8(f.readAll());
    *ok = true;
    return s;
}

/* Print the first differing line, which is far more useful than "they differ". */
static void reportDiff(const QString &got, const QString &want)
{
    const QStringList g = got.split(QLatin1Char('\n'));
    const QStringList w = want.split(QLatin1Char('\n'));
    const int n = qMax(g.size(), w.size());
    for (int i = 0; i < n; ++i) {
        const QString gl = i < g.size() ? g.at(i) : QString::fromLatin1("<missing>");
        const QString wl = i < w.size() ? w.at(i) : QString::fromLatin1("<missing>");
        if (gl != wl) {
            std::printf("  first difference at line %d:\n", i + 1);
            std::printf("    want: %s\n", wl.toUtf8().constData());
            std::printf("    got : %s\n", gl.toUtf8().constData());
            return;
        }
    }
    std::printf("  lines match; difference must be trailing whitespace\n");
    std::printf("  want %d bytes, got %d bytes\n", want.size(), got.size());
}

/*
 * Two active stations, which the golden files cannot cover: the original
 * station only ever had one, so its output never showed the separator.
 *
 * `sounders` is a list of lists. Written one per line with no comma, Python's
 * implicit line joining turns [a]\n[b] into [a][b] -- a SUBSCRIPT. That
 * parses cleanly, so nothing complains at read time; it simply is not a
 * literal, and rx_dechirp's ast.literal_eval drops the key and reports
 * "has no 'sounders'" while naming every other key in the file.
 *
 * Asserting on the separator rather than on a golden file, because there is
 * no original output for two stations to compare against.
 */
static bool twoStationsAreSeparated(const QString &dir)
{
    const QString path = dir + QLatin1String("/two_stations.ini");
    QFile::remove(path);
    {
        QSettings sched(path, QSettings::IniFormat);
        const char *const names[] = { "NIC4_280", "NIC_900" };
        for (int i = 0; i < 2; ++i) {
            sched.beginGroup(QLatin1String(names[i]));
            sched.setValue(QLatin1String("active"), true);
            sched.setValue(QLatin1String("rx"), QLatin1String("false"));
            sched.setValue(QLatin1String("cf"), 20000 + i);
            sched.setValue(QLatin1String("rep"), 300);
            sched.setValue(QLatin1String("lat"), 35);
            sched.setValue(QLatin1String("lon"), 34);
            sched.endGroup();
        }
        sched.sync();
    }

    QSettings sched(path, QSettings::IniFormat);
    QSettings empty(dir + QLatin1String("/empty_general.ini"), QSettings::IniFormat);
    const QString got = buildChirpConfig(sched, empty);

    const int opens = got.count(QLatin1String("[{'name':'"));
    const int seps  = got.count(QLatin1String("}],"));

    std::printf("two active stations:\n");
    std::printf("  %d station entr%s, %d separator%s\n",
                opens, opens == 1 ? "y" : "ies", seps, seps == 1 ? "" : "s");

    if (opens != 2) {
        std::printf("  FAIL: expected 2 station entries\n");
        return false;
    }
    if (seps != opens - 1) {
        std::printf("  FAIL: %d entries need %d separators, found %d.\n",
                    opens, opens - 1, seps);
        std::printf("  Without them Python reads [a][b] as a subscript and\n");
        std::printf("  rx_dechirp reports \"has no 'sounders'\".\n");
        return false;
    }
    std::printf("  separated correctly\n");
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString dir = QString::fromLocal8Bit(argc > 1 ? argv[1] : "tests/golden");

    QSettings schedule(dir + QLatin1String("/schedule.ini"), QSettings::IniFormat);
    QSettings config(dir + QLatin1String("/config.ini"), QSettings::IniFormat);

    bool ok = false;
    const QString want = readAll(dir + QLatin1String("/chirp_config.py"), &ok);
    if (!ok) {
        std::printf("FAIL: cannot read golden chirp_config.py from %s\n",
                    dir.toUtf8().constData());
        return 1;
    }

    const QString got = buildChirpConfig(schedule, config);

    int failures = 0;

    std::printf("chirp_config.py vs original binary output:\n");
    if (got == want) {
        std::printf("  byte-identical (%d bytes)\n", got.size());
    } else {
        reportDiff(got, want);
        std::printf("\n--- got ---\n%s\n--- end ---\n", got.toUtf8().constData());
        ++failures;
    }

    std::printf("\n");
    if (!twoStationsAreSeparated(QDir::tempPath()))
        ++failures;

    if (failures == 0) {
        std::printf("\nPASSED (0 failures)\n");
        return 0;
    }
    std::printf("\nFAILED (%d failure%s)\n", failures, failures == 1 ? "" : "s");
    return 1;
}

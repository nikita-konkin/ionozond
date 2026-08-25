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

    std::printf("chirp_config.py vs original binary output:\n");
    if (got == want) {
        std::printf("  byte-identical (%d bytes)\n", got.size());
        std::printf("\nPASSED (0 failures)\n");
        return 0;
    }

    reportDiff(got, want);
    std::printf("\n--- got ---\n%s\n--- end ---\n", got.toUtf8().constData());
    std::printf("\nFAILED (1 failure)\n");
    return 1;
}

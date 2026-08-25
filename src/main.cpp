#include "common.h"
#include "frmmain.h"

#include <QApplication>
#include <QDir>
#include <QFile>

/*
 * On first run the original ships default settings as Qt resources
 * (:/ini/config.ini and :/ini/schedule.ini, extracted from the binary's
 * resource bundle). Copy them out if the user has none yet.
 */
static void installDefaultSettings()
{
    const QString dir = homePath() + QLatin1String("/.config/dsChirp");
    QDir().mkpath(dir);

    struct { const char *res; QString dest; } files[] = {
        { ":/ini/config.ini",   configIniPath() },
        { ":/ini/schedule.ini", scheduleIniPath() },
    };

    for (int i = 0; i < 2; ++i) {
        if (QFile::exists(files[i].dest))
            continue;
        QFile::copy(QLatin1String(files[i].res), files[i].dest);
        QFile::setPermissions(files[i].dest,
                              QFile::ReadOwner | QFile::WriteOwner);
    }
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setOrganizationName(QLatin1String("volgatech"));
    a.setApplicationName(QLatin1String("dsChirp"));

    installDefaultSettings();
    qInstallMessageHandler(myMessageHandler);

    frmMain w;
    w.show();
    return a.exec();
}

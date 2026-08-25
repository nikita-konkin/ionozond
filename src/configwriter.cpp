#include "configwriter.h"

#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QTextStream>

namespace {

/* Keys of the sounder dict whose value is in kHz in the .ini and has to be
 * emitted in Hz, which the original does by appending an "e3" suffix. */
bool needsE3(const QString &key)
{
    return key == QLatin1String("cf") || key == QLatin1String("rate");
}

/* Python-ise a settings value: booleans get capitalised, everything else is
 * passed through untouched (including empty values -- the original happily
 * writes "tb = " when the key exists but is empty). */
QString pythonValue(const QString &value)
{
    if (value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
        return QLatin1String("True");
    if (value.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
        return QLatin1String("False");
    return value;
}

const char *const TRAILER =
    "\ndef get_all_sounders():\n"
    "    sounder_list = []\n"
    "    for sounder_thread in sounders:\n"
    "        for sounder in sounder_thread:\n"
    "            sounder_list.append(sounder)\n"
    "    return(sounder_list)";

} // namespace

QString buildChirpConfig(QSettings &schedule, QSettings &config)
{
    QStringList lines;

    /* --- the sounder table ------------------------------------------------ */
    lines << QLatin1String("sounders = [\n");

    QString rxStation;
    const QStringList stations = schedule.childGroups();

    for (int i = 0; i < stations.size(); ++i) {
        const QString &station = stations.at(i);
        schedule.beginGroup(station);

        if (schedule.value(QLatin1String("rx")).toString()
                .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
            rxStation = QString(QLatin1String("rx_station = {'name':'%1','lat':%2,'lon':%3}"))
                            .arg(station)
                            .arg(schedule.value(QLatin1String("lat")).toString())
                            .arg(schedule.value(QLatin1String("lon")).toString());
        }

        if (schedule.value(QLatin1String("active")).toBool()) {
            QString line = QLatin1String("[{'name':'");
            line += station;
            line += QLatin1Char('\'');

            /* childKeys() is sorted, which is what fixes the key order. */
            const QStringList keys = schedule.childKeys();
            for (int k = 0; k < keys.size(); ++k) {
                const QString &key = keys.at(k);
                if (key == QLatin1String("active"))
                    continue;

                QString value = pythonValue(schedule.value(key).toString());
                if (needsE3(key))
                    value += QLatin1String("e3");

                line += QLatin1Char(',');
                line += QLatin1Char('\'');
                line += key;
                line += QLatin1String("':");
                line += value;
            }
            line += QLatin1String("}]\n");
            lines << line;
        }

        schedule.endGroup();
    }

    lines << QLatin1String("]\n");
    lines << QLatin1String("\n");
    lines << rxStation + QLatin1String("\n");
    lines << QLatin1String("\n");

    /* --- the [General] settings ------------------------------------------- */
    /* QSettings maps the [General] section onto the root, so childKeys() here
     * returns exactly the [General] keys (and [Variations] is not emitted). */
    const QStringList keys = config.childKeys();
    for (int i = 0; i < keys.size(); ++i) {
        const QString &key = keys.at(i);

        /* dsChirp's own settings, not the sounder's. */
        if (key == QLatin1String("config_file") || key == QLatin1String("sound_app"))
            continue;
        /* Emitted as an expression next to sample_rate instead. */
        if (key == QLatin1String("if_rate"))
            continue;

        QString value = pythonValue(config.value(key).toString());
        if (key == QLatin1String("data_dir"))
            value = QLatin1Char('"') + value + QLatin1Char('"');
        else if (key == QLatin1String("sample_rate"))
            value += QLatin1String("e3");

        lines << key + QLatin1String(" = ") + value + QLatin1String("\n");

        /* if_rate always follows sample_rate, out of alphabetical order. */
        if (key == QLatin1String("sample_rate"))
            lines << QLatin1String("if_rate = sample_rate/dec\n");
    }

    lines << QLatin1String(TRAILER);

    return lines.join(QString());
}

bool writeChirpConfig(const QString &path, QSettings &schedule, QSettings &config)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out << buildChirpConfig(schedule, config);
    const bool ok = (out.status() == QTextStream::Ok);
    file.close();
    return ok;
}

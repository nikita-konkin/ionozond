#ifndef SCHEDULECHECK_H
#define SCHEDULECHECK_H

#include <QList>
#include <QString>
#include <QVector>

class QSettings;

/*
 * Checks on the schedule and the parameters that the console would otherwise
 * accept without a word, each of them found the expensive way or read off
 * the sounder's code:
 *
 *   - stations whose soundings collide. One radio follows one sweep at a time
 *     and the sounder takes whichever is due first, so a station due while
 *     another is being recorded -- or within the 3 s the sounder needs to get
 *     ready -- is simply never recorded, and nothing anywhere says so;
 *   - values chirp_config.py cannot carry. They are copied into it verbatim,
 *     so "56,38" or a name with a quote in it is a Python syntax error, and a
 *     Cyrillic name crashes the sounder when it writes the capture header;
 *   - settings that are legal but throw data away: a sweep longer than the
 *     capture band, a delay window past the IF band, a speckle filter that
 *     deletes every cell.
 *
 * Kept free of widgets so it can be tested, and so the schedule dialog, the
 * parameters dialog and START all ask the same questions.
 */

struct ScheduleEntry {
    QString name;
    bool    active;
    bool    rx;
    /* As typed: the checks have to see "56,38" to say what is wrong with it. */
    QString rep, chirpt, rate, dur, cf, lat, lon;
    int     fft;        // 0 = the parameters dialog's

    ScheduleEntry() : active(false), rx(false), fft(0) {}
};

struct CheckConfig {
    double  sampleRateKHz;    // sample_rate, kHz as in config.ini
    int     sampleRateIndex;  // -1 when absent
    double  dec;
    int     fft;              // the general FFT length, points
    int     objW, objH;
    double  objLevel;
    QString dataDir;
    QString soundApp;
    bool    keepLfs;
    double  keepHours;        // 0 = for ever
    /* Look at the disk: data_dir, the sounding program, free space. Off in
     * the tests. */
    bool    checkFiles;

    CheckConfig()
        : sampleRateKHz(25000), sampleRateIndex(-1), dec(625), fft(16384),
          objW(9), objH(3), objLevel(11), keepLfs(false), keepHours(0),
          checkFiles(true) {}

    double ifRateHz() const { return dec > 0 ? sampleRateKHz * 1e3 / dec : 0.0; }
};

struct CheckIssue {
    enum Level { Error = 0, Warning, Info };
    Level   level;
    QString station;   // empty for a general setting
    QString text;

    CheckIssue(Level l = Info, const QString &s = QString(), const QString &t = QString())
        : level(l), station(s), text(t) {}
};

QList<CheckIssue> checkSchedule(const QList<ScheduleEntry> &entries, const CheckConfig &cfg);

QList<ScheduleEntry> readScheduleEntries(QSettings &schedule);
CheckConfig readCheckConfig(QSettings &config);

int countIssues(const QList<CheckIssue> &issues, CheckIssue::Level level);
/* One line per issue, worst first; rich text for a label or a message box. */
QString issuesHtml(const QList<CheckIssue> &issues);
QString issueLine(const CheckIssue &issue);

/*
 * The sounder's own choice of what to record, replayed: rx_dechirp.py's
 * determine_next (the grid restarts at each UTC midnight, and a chirp less
 * than 3 s away is already too late) and pick_next (the earliest wins, a tie
 * goes to the station listed first), with `processingS` spent after each
 * capture on the sidecar before the next choice is made.
 */
struct SimStation {
    double rep, chirpt, dur;
    double post;     // s after this station's capture before the next choice
};
struct SimCapture {
    int    station;
    double start;    // s from the first simulated midnight
};
/* Captures that start in [fromS, toS), simulated from midnight 0. */
QVector<SimCapture> simulateSounder(const QVector<SimStation> &stations,
                                    double fromS, double toS);

/* The sounder's guess at the sidecar and archive time after a capture: about
 * 1.5 s for a 52 s capture measured, so this is generous on purpose. */
inline double processingEstimateS(double durS) { return 2.0 + 0.06 * durS; }

/* The lead the sounder needs before a chirp (determine_next). */
const double SOUNDER_LEAD_S = 3.0;

#endif /* SCHEDULECHECK_H */

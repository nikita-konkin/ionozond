#include "schedulecheck.h"

#include "common.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

const double DAY_S = 86400.0;
const double EPS = 1e-6;
/* rx_dechirp.py --min-free-gb: a window is skipped unless this much is left
 * over after the capture. */
const double SOUNDER_FLOOR_BYTES = 5.0 * 1073741824.0;
const double LFS_HEADER_BYTES = 512.0;

QString u8(const char *s) { return QString::fromUtf8(s); }

/* A number as the operator typed it, echoed back. */
QString val(double v) { return QString::number(v, 'g', 10); }
QString fixed(double v, int prec) { return QString::number(v, 'f', prec); }

void add(QList<CheckIssue> &out, CheckIssue::Level level, const QString &station,
         const QString &text)
{
    out << CheckIssue(level, station, text);
}

/*
 * A value as chirp_config.py will receive it. configwriter copies the text in
 * verbatim, so it has to be a Python number exactly as typed: "56,38" inside
 * a dict literal is a syntax error, and so is 0300.
 */
struct Number {
    bool    ok;
    double  v;
    QString problem;
};

Number parseNumber(const QString &text)
{
    Number n;
    n.ok = false;
    n.v = 0.0;
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        n.problem = u8("не задано");
        return n;
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || !std::isfinite(v)) {
        QString dotted = t;
        dotted.replace(QLatin1Char(','), QLatin1Char('.'));
        bool dottedOk = false;
        dotted.toDouble(&dottedOk);
        if (t.contains(QLatin1Char(',')) && dottedOk)
            n.problem = u8("«%1»: дробная часть отделяется точкой, а не запятой").arg(t);
        else
            n.problem = u8("«%1» — не число").arg(t);
        return n;
    }
    static const QRegularExpression leadingZero(QLatin1String("^[+-]?0+[1-9][0-9]*$"));
    if (leadingZero.match(t).hasMatch()) {
        n.problem = u8("«%1»: Python не принимает целое число с ведущим нулём").arg(t);
        return n;
    }
    n.ok = true;
    n.v = v;
    return n;
}

QString rawValue(const QVariant &v)
{
    /* An unquoted "56,38" in the .ini reads back as a list. configwriter's
     * toString() then writes nothing at all, so show it as it was typed. */
    if (v.userType() == QMetaType::QStringList)
        return v.toStringList().join(QLatin1String(","));
    return v.toString();
}

/* ------------------------------------------------------------------ names */

void checkName(const ScheduleEntry &e, QList<CheckIssue> &out)
{
    const QString &n = e.name;
    /* Only active stations and the receiver reach chirp_config.py and the
     * capture header; for the rest it is a warning about later. */
    const CheckIssue::Level used = (e.active || e.rx) ? CheckIssue::Error : CheckIssue::Warning;

    if (n.trimmed().isEmpty()) {
        add(out, CheckIssue::Error, n, u8("пустое название станции"));
        return;
    }
    if (n.contains(QLatin1Char('/')) || n.contains(QLatin1Char('\\')))
        add(out, CheckIssue::Error, n,
            u8("«/» и «\\» в названии ломают schedule.ini и пути к записям"));
    if (n.contains(QLatin1Char('\'')) || n.contains(QLatin1Char('"')))
        add(out, used, n, u8("кавычка в названии: chirp_config.py не прочитается"));

    bool control = false, wide = false;
    for (int i = 0; i < n.size(); ++i) {
        const ushort c = n.at(i).unicode();
        if (c < 0x20 || c == 0x7f)
            control = true;
        if (c > 0xff)
            wide = true;
    }
    if (control)
        add(out, CheckIssue::Error, n, u8("управляющий символ в названии"));
    if (wide)
        add(out, used, n, u8("название не латиницей: зондировщик пишет его в заголовок "
                             "записи в Latin-1, и запись не состоится. Используйте "
                             "латинские буквы, например Norilsk."));
    else if (n.toLatin1().size() > 64)
        add(out, CheckIssue::Warning, n,
            u8("название длиннее 64 символов: в заголовок записи попадёт обрезанным"));
}

/* Coordinates of one station; false when they cannot be used for a path. */
bool parseCoords(const ScheduleEntry &e, double *lat, double *lon, QList<CheckIssue> &out)
{
    const Number la = parseNumber(e.lat);
    const Number lo = parseNumber(e.lon);
    bool ok = true;
    if (!la.ok) {
        add(out, CheckIssue::Error, e.name, u8("широта: %1").arg(la.problem));
        ok = false;
    } else if (std::fabs(la.v) > 90.0) {
        add(out, CheckIssue::Error, e.name, u8("широта %1 вне −90…90").arg(val(la.v)));
        ok = false;
    }
    if (!lo.ok) {
        add(out, CheckIssue::Error, e.name, u8("долгота: %1").arg(lo.problem));
        ok = false;
    } else if (std::fabs(lo.v) > 180.0) {
        add(out, CheckIssue::Error, e.name, u8("долгота %1 вне −180…180").arg(val(lo.v)));
        ok = false;
    }
    if (ok && la.v == 0.0 && lo.v == 0.0)
        add(out, CheckIssue::Warning, e.name,
            u8("координаты 0, 0 — не заданы? По ним строится окно задержки ионограммы."));
    *lat = la.v;
    *lon = lo.v;
    return ok;
}

/* ---------------------------------------------------------------- general */

void checkGeneral(const CheckConfig &cfg, QList<CheckIssue> &out)
{
    if (!(cfg.sampleRateKHz > 0.0))
        add(out, CheckIssue::Error, QString(), u8("частота дискретизации не задана"));
    if (!(cfg.dec > 0.0))
        add(out, CheckIssue::Error, QString(),
            u8("коэффициент децимации должен быть больше нуля"));
    else if (std::fabs(cfg.dec - std::floor(cfg.dec)) > 1e-9)
        add(out, CheckIssue::Warning, QString(),
            u8("децимация %1 не целая: зондировщик возьмёт %2")
                .arg(val(cfg.dec)).arg(fixed(std::floor(cfg.dec), 0)));

    const QStringList &rates = SAMPLE_RATE_LIST();
    if (cfg.sampleRateIndex >= 0 && cfg.sampleRateIndex < rates.size()
            && std::fabs(rates.at(cfg.sampleRateIndex).toDouble() - cfg.sampleRateKHz) > 1e-6)
        add(out, CheckIssue::Warning, QString(),
            u8("частота дискретизации в config.ini записана дважды и по-разному "
               "(%1 и %2 кГц). Откройте окно параметров и сохраните его.")
                .arg(val(cfg.sampleRateKHz)).arg(rates.at(cfg.sampleRateIndex)));

    /* deleteSmallObjects keeps a cell when at least obj_level of the cells in
     * the window around it are set; the window is odd in both directions. */
    const int w = 2 * (std::max(cfg.objW, 0) / 2) + 1;
    const int h = 2 * (std::max(cfg.objH, 0) / 2) + 1;
    if (cfg.objLevel > w * h)
        add(out, CheckIssue::Error, QString(),
            u8("порог фильтра мелких объектов %1 больше числа клеток в его окне "
               "%2 × %3 = %4: с ионограммы будет удалено всё")
                .arg(val(cfg.objLevel)).arg(w).arg(h).arg(w * h));
}

/* ---------------------------------------------------------------- station */

struct Station {
    QString name;
    double  rep, chirpt, rateKHz, dur, cfKHz;
};

void checkStation(const Station &s, int fft, const CheckConfig &cfg,
                  bool pathOk, double txLat, double txLon, double rxLat, double rxLon,
                  QList<CheckIssue> &out)
{
    const QString &n = s.name;
    const double ifRate = cfg.ifRateHz();
    const double srHz = cfg.sampleRateKHz * 1e3;
    const double rateHz = s.rateKHz * 1e3;

    /* Units: cf in kHz and rate in kHz/s, as the original's table had them. */
    if (s.cfKHz < 1000.0)
        add(out, CheckIssue::Warning, n,
            u8("частота гетеродина %1 кГц — меньше 1 МГц. Она задаётся в кГц: "
               "12,5 МГц — это 12500.").arg(val(s.cfKHz)));
    if (s.rateKHz * s.dur < 1000.0)
        add(out, CheckIssue::Warning, n,
            u8("развёртка всего %1 кГц (скорость × продолжительность). Скорость "
               "задаётся в кГц/с: 100 кГц/с — это 100.").arg(val(s.rateKHz * s.dur)));

    /* The capture band is fs wide and the sweep starts at its bottom edge.
     * Half a second of slack: Sodankyla's 500.0084 kHz/s over 50 s is past
     * the edge by under a millisecond. */
    if (srHz > 0.0 && rateHz > 0.0 && s.dur - srHz / rateHz >= 0.5) {
        const double fits = srHz / rateHz;
        add(out, CheckIssue::Warning, n,
            u8("развёртка %1 с × %2 кГц/с = %3 МГц шире полосы приёма %4 МГц: "
               "последние %5 с записываются вне полосы, это шум. Довольно %6 с.")
                .arg(val(s.dur)).arg(val(s.rateKHz))
                .arg(fixed(rateHz * s.dur / 1e6, 1)).arg(fixed(srHz / 1e6, 1))
                .arg(fixed(s.dur - fits, 1)).arg(fixed(std::floor(fits + 1e-9), 0)));
    }
    if (srHz > 0.0 && s.cfKHz * 1e3 - srHz / 2.0 < 0.0)
        add(out, CheckIssue::Warning, n,
            u8("развёртка начиналась бы с %1 МГц, ниже нуля: гетеродин ставится на "
               "начальную частоту передатчика плюс половину полосы (%2 МГц).")
                .arg(fixed((s.cfKHz * 1e3 - srHz / 2.0) / 1e6, 2))
                .arg(fixed(srHz / 2e6, 3)));

    /* rx_dechirp counts sessions from UTC midnight (midnight + chirpt + k*rep),
     * the console's session panel from the epoch ((t % rep) == chirpt). */
    if (s.chirpt < 0.0 || s.chirpt >= s.rep) {
        const double fold = s.chirpt - std::floor(s.chirpt / s.rep) * s.rep;
        add(out, CheckIssue::Warning, n,
            u8("chirptime %1 вне 0…%2 с: зондировщик пропустит сеансы в начале "
               "суток, а консоль покажет другое время. Это тот же сеанс, что "
               "chirptime %3.").arg(val(s.chirpt)).arg(val(s.rep)).arg(val(fold)));
    }
    if (std::fabs(DAY_S / s.rep - std::floor(DAY_S / s.rep + 0.5)) > 1e-9)
        add(out, CheckIssue::Warning, n,
            u8("сутки не делятся на период %1 с: зондировщик начинает сетку сеансов "
               "заново в полночь UTC, консоль ведёт её непрерывно, и в течение "
               "суток они расходятся.").arg(val(s.rep)));

    /* The delay window has to fit in the IF band: a beat past if_rate/2
     * folds back onto another delay. */
    if (pathOk && ifRate > 0.0 && rateHz > 0.0) {
        const double dKm = earthDistanceKm(txLat, txLon, rxLat, rxLon);
        const double tauMaxS = (rayDistanceKm(dKm) + VIRT_HEIGHT_WINDOW_KM_DEFAULT)
                               / LIGHT_SPEED_KM_S;
        const double reachS = ifRate / 2.0 / rateHz;
        if (tauMaxS > reachS)
            add(out, CheckIssue::Error, n,
                u8("окно задержки доходит до %1 мс (трасса %2 км), а при %3 кГц/с "
                   "и полосе ±%4 кГц после децимации различимо не больше %5 мс: "
                   "отражения наложатся на чужие задержки. Уменьшите скорость "
                   "или децимацию.")
                    .arg(fixed(tauMaxS * 1e3, 1)).arg(fixed(dKm, 0))
                    .arg(val(s.rateKHz)).arg(fixed(ifRate / 2e3, 1))
                    .arg(fixed(reachS * 1e3, 1)));
    }

    /* The analysis FFT against the capture. */
    if (ifRate > 0.0 && fft > 0 && rateHz > 0.0) {
        const double samples = ifRate * s.dur;
        const double spectra = std::floor(samples / fft);
        const double delayRows = rateHz
            * ((VIRT_HEIGHT_WINDOW_KM_DEFAULT + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT)
               / LIGHT_SPEED_KM_S) * fft / ifRate;
        if (spectra < 1.0)
            add(out, CheckIssue::Error, n,
                u8("БПФ %1 точек длиннее всей записи (%2 отсчётов): ионограммы не будет")
                    .arg(fft).arg(fixed(samples, 0)));
        else if (spectra <= 2 * (std::max(cfg.objW, 0) / 2))
            add(out, CheckIssue::Error, n,
                u8("всего %1 спектров по частоте, а окно фильтра мелких объектов %2: "
                   "он удалит всё. Уменьшите БПФ.").arg(fixed(spectra, 0)).arg(cfg.objW));
        else if (spectra < 60.0)
            add(out, CheckIssue::Warning, n,
                u8("всего %1 спектров по частоте (у оригинала 610): ионограмма будет "
                   "грубой. Уменьшите БПФ.").arg(fixed(spectra, 0)));
        if (delayRows < 30.0)
            add(out, CheckIssue::Warning, n,
                u8("окно задержки занимает всего %1 строк БПФ: ионограмма будет "
                   "грубой. Увеличьте БПФ.").arg(fixed(delayRows, 1)));

        /* A cell is rate*N/if_rate of sweep by its reciprocal of delay, so
         * the original's 41 kHz x 7.3 km needs rate*N/if_rate held fixed. */
        const double ideal = 16384.0 * (100e3 / rateHz) * (ifRate / 40000.0);
        int rec = 1 << (int)std::floor(std::log(ideal) / std::log(2.0) + 0.5);
        rec = std::max(512, std::min(65536, rec));
        if (fft >= 4 * rec || rec >= 4 * fft) {
            const double sweep = rateHz * fft / ifRate;
            const double recSweep = rateHz * rec / ifRate;
            add(out, CheckIssue::Info, n,
                u8("ячейка ионограммы %1 кГц × %2 км (у оригинала 41 × 7,3). При "
                   "%3 кГц/с форму оригинала даёт БПФ %4: %5 × %6.")
                    .arg(fixed(sweep / 1e3, 0)).arg(fixed(LIGHT_SPEED_KM_S / sweep, 1))
                    .arg(val(s.rateKHz)).arg(rec)
                    .arg(fixed(recSweep / 1e3, 0))
                    .arg(fixed(LIGHT_SPEED_KM_S / recSweep, 1)));
        }
    }
}

/* ------------------------------------------------------------- collisions */

QVector<SimCapture> captures(const QVector<SimStation> &s, double from)
{
    return simulateSounder(s, from, 2.0 * DAY_S);
}

QVector<SimStation> one(const SimStation &s)
{
    return QVector<SimStation>() << s;
}

/* Sessions of `victim` that it would have recorded on its own and does not
 * with the others, keyed by the station recording when it came. */
struct Loss {
    int    count;
    double maxDur;   // the longest the culprit could record and spare them all
    double minGap;   // the least time from the culprit's end to the session
    Loss() : count(0), maxDur(1e300), minGap(1e300) {}
};
typedef QMap<QPair<int, int>, Loss> LossMap;   // (victim, culprit)

LossMap lossesOf(const QVector<SimStation> &s, QVector<int> *alone, QVector<int> *together)
{
    LossMap out;
    /* From midnight 0, so the capture holding the radio at the start of day 1
     * is there to be blamed. */
    const QVector<SimCapture> all = captures(s, 0.0);
    std::vector<double> starts;
    QSet<qint64> recorded;
    for (int k = 0; k < all.size(); ++k) {
        starts.push_back(all.at(k).start);
        recorded.insert((qint64)std::floor(all.at(k).start * 1000.0 + 0.5) * 64
                        + all.at(k).station);
        if (together && all.at(k).start >= DAY_S)
            ++(*together)[all.at(k).station];
    }

    for (int i = 0; i < s.size(); ++i) {
        const QVector<SimCapture> own = captures(one(s.at(i)), DAY_S);
        if (alone)
            (*alone)[i] = own.size();
        for (int k = 0; k < own.size(); ++k) {
            const double t = own.at(k).start;
            if (recorded.contains((qint64)std::floor(t * 1000.0 + 0.5) * 64 + i))
                continue;
            /* The last capture to start no later than this session. */
            const int at = (int)(std::upper_bound(starts.begin(), starts.end(), t + EPS)
                                 - starts.begin()) - 1;
            if (at < 0)
                continue;
            const SimCapture &c = all.at(at);
            const SimStation &cs = s.at(c.station);
            if (c.station == i || c.start + cs.dur + cs.post <= t - SOUNDER_LEAD_S - EPS)
                continue;
            Loss &l = out[qMakePair(i, c.station)];
            ++l.count;
            l.maxDur = std::min(l.maxDur, t - SOUNDER_LEAD_S - cs.post - c.start);
            l.minGap = std::min(l.minGap, t - (c.start + cs.dur));
        }
    }
    return out;
}

/* A shorter recording of `culprit` that spares `victim`, or 0. */
double shorterDur(const QVector<SimStation> &s, int victim, int culprit, double maxDur)
{
    const double d = std::floor(maxDur + EPS);
    if (d < 1.0 || d >= s.at(culprit).dur)
        return 0.0;
    QVector<SimStation> tried = s;
    tried[culprit].dur = d;
    tried[culprit].post = processingEstimateS(d);
    const LossMap after = lossesOf(tried, 0, 0);
    if (after.contains(qMakePair(victim, culprit)) || after.contains(qMakePair(culprit, victim)))
        return 0.0;
    return d;
}

/* `culprit` recorded every other session -- its period doubled, on either of
 * the two grids that leaves -- if that spares `victim`. The chirptime, or -1. */
double sparserGrid(const QVector<SimStation> &s, int victim, int culprit)
{
    for (int k = 0; k < 2; ++k) {
        QVector<SimStation> tried = s;
        tried[culprit].rep = 2.0 * s.at(culprit).rep;
        tried[culprit].chirpt = s.at(culprit).chirpt + k * s.at(culprit).rep;
        const LossMap after = lossesOf(tried, 0, 0);
        if (!after.contains(qMakePair(victim, culprit))
                && !after.contains(qMakePair(culprit, victim)))
            return tried.at(culprit).chirpt;
    }
    return -1.0;
}

/*
 * What would let `victim` be recorded too, as the end of a sentence, or empty.
 * A shorter capture is offered only while it still sweeps 5 MHz: cutting a
 * 52 s Magadan sweep to 6 s to fit a Cyprus session in is no answer. Recording
 * the culprit every other session usually is.
 */
QString fixFor(const QVector<SimStation> &est, const QList<Station> &st,
               int victim, int culprit, double maxDur)
{
    const Station &c = st.at(culprit);
    const double d = shorterDur(est, victim, culprit, maxDur);
    const bool cut = d > 0.0 && d * c.rateKHz >= 5000.0;
    const double grid = sparserGrid(est, victim, culprit);

    QStringList ways;
    if (cut)
        ways << u8("сократите продолжительность у %1 до %2 с").arg(c.name).arg(fixed(d, 0));
    if (grid >= 0.0)
        ways << u8("записывайте %1 через сеанс (период %2 с, chirptime %3)")
                    .arg(c.name).arg(val(2.0 * c.rep)).arg(val(grid));
    return ways.join(u8(" или "));
}

QVector<int> checkCollisions(const QList<Station> &st, QList<CheckIssue> &out)
{
    const int n = st.size();
    QVector<SimStation> zero(n), est(n);
    for (int i = 0; i < n; ++i) {
        SimStation s;
        s.rep = st.at(i).rep;
        s.chirpt = st.at(i).chirpt;
        s.dur = st.at(i).dur;
        s.post = 0.0;
        zero[i] = s;
        s.post = processingEstimateS(s.dur);
        est[i] = s;
    }

    QVector<int> nominal(n, 0), alone0(n, 0), aloneE(n, 0), perDay(n, 0);
    bool reported = false;

    /* A station against itself: a session, then the lead, must fit in a
     * period, or every other one goes. */
    for (int i = 0; i < n; ++i) {
        SimStation grid = zero.at(i);
        grid.dur = 0.0;
        nominal[i] = captures(one(grid), DAY_S).size();
        alone0[i] = captures(one(zero.at(i)), DAY_S).size();
        aloneE[i] = captures(one(est.at(i)), DAY_S).size();
        const Station &s = st.at(i);
        if (alone0.at(i) < nominal.at(i)) {
            add(out, CheckIssue::Warning, s.name,
                u8("сеанс %1 с и ещё 3 с на подготовку не помещаются в период %2 с: "
                   "записывается %3 из %4 сеансов в сутки.")
                    .arg(val(s.dur)).arg(val(s.rep))
                    .arg(alone0.at(i)).arg(nominal.at(i)));
            reported = true;
        } else if (aloneE.at(i) < nominal.at(i)) {
            add(out, CheckIssue::Warning, s.name,
                u8("после сеанса до следующего остаётся %1 с, а на обработку записи "
                   "и подготовку нужно около %2 с: может записываться %3 из %4 "
                   "сеансов в сутки.")
                    .arg(fixed(s.rep - s.dur, 0))
                    .arg(fixed(est.at(i).post + SOUNDER_LEAD_S, 0))
                    .arg(aloneE.at(i)).arg(nominal.at(i)));
            reported = true;
        }
    }

    const LossMap definite = lossesOf(zero, 0, 0);
    const LossMap risky = lossesOf(est, 0, &perDay);

    for (LossMap::const_iterator it = definite.constBegin(); it != definite.constEnd(); ++it) {
        const int v = it.key().first, c = it.key().second;
        const Loss &l = it.value();
        /* Advice from the realistic run: a cut that only clears the lead
         * would still lose them to the sidecar. */
        const double maxDur = risky.contains(it.key()) ? risky.value(it.key()).maxDur : l.maxDur;
        QString advice = fixFor(est, st, v, c, maxDur);
        if (!advice.isEmpty())
            advice = u8(" Чтобы записывать обе, %1.").arg(advice);
        else if (l.maxDur < 1.0)
            advice = u8(" Сеансы начинаются почти одновременно: одним приёмником их не "
                        "принять, оставьте активной одну из станций.");
        else
            advice = u8(" Оставьте активной одну из станций.");
        add(out, CheckIssue::Warning, st.at(v).name,
            u8("теряет %1 из %2 сеансов в сутки: в это время записывается %3.%4")
                .arg(l.count).arg(alone0.at(v)).arg(st.at(c).name).arg(advice));
        reported = true;
    }

    for (LossMap::const_iterator it = risky.constBegin(); it != risky.constEnd(); ++it) {
        const int v = it.key().first, c = it.key().second;
        const Loss &l = it.value();
        if (definite.contains(it.key()) && definite.value(it.key()).count >= l.count)
            continue;
        const QString advice = fixFor(est, st, v, c, l.maxDur);
        add(out, CheckIssue::Warning, st.at(v).name,
            u8("может терять до %1 из %2 сеансов в сутки: запись %3 кончается за %4 с "
               "до его начала, а на обработку записи и подготовку нужно около %5 с.%6")
                .arg(l.count).arg(aloneE.at(v)).arg(st.at(c).name)
                .arg(fixed(std::max(0.0, l.minGap), 0))
                .arg(fixed(est.at(c).post + SOUNDER_LEAD_S, 0))
                .arg(advice.isEmpty() ? QString() : u8(" Надёжнее %1.").arg(advice)));
        reported = true;
    }

    if (n >= 2 && !reported)
        add(out, CheckIssue::Info, QString(),
            u8("сеансы %1 станций не пересекаются: проверены сутки, учтены 3 с "
               "на подготовку и время на обработку каждой записи").arg(n));
    return perDay;
}

/* ------------------------------------------------------------------ files */

QString existingAncestor(const QString &path)
{
    QString p = QDir::cleanPath(QDir(path).absolutePath());
    while (!QFileInfo(p).exists()) {
        const int k = p.lastIndexOf(QLatin1Char('/'));
        if (k <= 0)
            return QLatin1String("/");
        p = p.left(k);
    }
    return p;
}

void checkFiles(const CheckConfig &cfg, const QList<Station> &st,
                const QVector<int> &perDay, QList<CheckIssue> &out)
{
    const QString app = cfg.soundApp.trimmed();
    if (app.isEmpty()) {
        add(out, CheckIssue::Error, QString(),
            u8("программа зондирования не задана: START ничего не запустит"));
    } else {
        const QFileInfo fi(app);
        const bool onPath = !fi.isAbsolute()
                            && !QStandardPaths::findExecutable(app).isEmpty();
        if (!fi.exists() && !onPath)
            add(out, CheckIssue::Error, QString(),
                u8("программа зондирования %1 не найдена").arg(app));
        else if (fi.exists() && !fi.isExecutable())
            add(out, CheckIssue::Warning, QString(),
                u8("%1 не исполняемый файл (chmod +x)").arg(app));
    }

    const QString dir = cfg.dataDir.trimmed();
    if (dir.isEmpty()) {
        add(out, CheckIssue::Error, QString(), u8("каталог данных не задан"));
        return;
    }
    const QFileInfo fi(dir);
    const QString base = existingAncestor(dir);
    if (!fi.exists()) {
        if (!QFileInfo(base).isWritable())
            add(out, CheckIssue::Error, QString(),
                u8("каталога данных %1 нет, и создать его в %2 нельзя: нет прав")
                    .arg(dir, base));
    } else if (!fi.isDir()) {
        add(out, CheckIssue::Error, QString(), u8("%1 — не каталог").arg(dir));
        return;
    } else if (!fi.isWritable()) {
        add(out, CheckIssue::Error, QString(),
            u8("нет прав на запись в каталог данных %1").arg(dir));
    }

    const QStorageInfo storage(base);
    if (!storage.isValid() || !storage.isReady() || st.isEmpty())
        return;
    const double freeBytes = (double)storage.bytesAvailable();
    const double ifRate = cfg.ifRateHz();
    double largest = 0.0, daily = 0.0;
    for (int i = 0; i < st.size(); ++i) {
        const double bytes = LFS_HEADER_BYTES + 8.0 * ifRate * st.at(i).dur;
        largest = std::max(largest, bytes);
        daily += bytes * (i < perDay.size() ? perDay.at(i) : 0);
    }
    const double gb = 1073741824.0;
    if (freeBytes < SOUNDER_FLOOR_BYTES + largest) {
        add(out, CheckIssue::Error, QString(),
            u8("на диске с данными свободно %1 ГБ: зондировщик пропускает сеансы, пока "
               "свободно меньше 5 ГБ сверх размера записи (%2 МБ)")
                .arg(fixed(freeBytes / gb, 1)).arg(fixed(largest / 1048576.0, 0)));
    } else if (cfg.keepLfs && daily > 0.0) {
        const double room = freeBytes - SOUNDER_FLOOR_BYTES;
        if (cfg.keepHours <= 0.0) {
            const double days = room / daily;
            if (days < 14.0)
                add(out, CheckIssue::Warning, QString(),
                    u8("записи .lfs хранятся бессрочно, около %1 ГБ в сутки: места "
                       "хватит примерно на %2 сут.")
                        .arg(fixed(daily / gb, 1)).arg(fixed(days, 1)));
        } else if (daily * cfg.keepHours / 24.0 > room) {
            add(out, CheckIssue::Warning, QString(),
                u8("хранение .lfs %1 ч требует около %2 ГБ, а свободно %3 ГБ сверх "
                   "запаса в 5 ГБ: зондировщик начнёт пропускать сеансы")
                    .arg(val(cfg.keepHours)).arg(fixed(daily * cfg.keepHours / 24.0 / gb, 1))
                    .arg(fixed(room / gb, 1)));
        }
    }
}

bool levelLess(const CheckIssue &a, const CheckIssue &b)
{
    return a.level < b.level;
}

} // namespace

/* ------------------------------------------------------------- simulation */

static double nextStart(const SimStation &s, double now)
{
    /* determine_next: t0 = midnight + chirpt; while t0 - now < 3: t0 += rep */
    const double midnight = std::floor(now / DAY_S) * DAY_S;
    const double first = midnight + s.chirpt;
    double t0 = first;
    if (t0 - now < SOUNDER_LEAD_S) {
        const double k = std::ceil((now + SOUNDER_LEAD_S - first) / s.rep);
        t0 = first + std::max(0.0, k) * s.rep;
        while (t0 - now < SOUNDER_LEAD_S)
            t0 += s.rep;
        while (t0 - s.rep >= first && t0 - s.rep - now >= SOUNDER_LEAD_S)
            t0 -= s.rep;
    }
    return t0;
}

QVector<SimCapture> simulateSounder(const QVector<SimStation> &stations,
                                    double fromS, double toS)
{
    QVector<SimCapture> out;
    if (stations.isEmpty())
        return out;
    for (int i = 0; i < stations.size(); ++i)
        if (!(stations.at(i).rep > 0.0))
            return out;

    double now = 0.0;
    while (now < toS) {
        /* pick_next: the earliest, the first listed on a tie. */
        int best = -1;
        double bestT = 0.0;
        for (int i = 0; i < stations.size(); ++i) {
            const double t = nextStart(stations.at(i), now);
            if (best < 0 || t < bestT) {
                best = i;
                bestT = t;
            }
        }
        if (bestT >= toS)
            break;
        if (bestT >= fromS) {
            SimCapture c;
            c.station = best;
            c.start = bestT;
            out << c;
        }
        now = bestT + std::max(0.0, stations.at(best).dur)
                    + std::max(0.0, stations.at(best).post);
    }
    return out;
}

/* ------------------------------------------------------------------ entry */

QList<CheckIssue> checkSchedule(const QList<ScheduleEntry> &entries, const CheckConfig &cfg)
{
    QList<CheckIssue> out;
    checkGeneral(cfg, out);

    QSet<QString> seen, twice;
    for (int i = 0; i < entries.size(); ++i) {
        checkName(entries.at(i), out);
        if (seen.contains(entries.at(i).name))
            twice.insert(entries.at(i).name);
        seen.insert(entries.at(i).name);
    }
    for (QSet<QString>::const_iterator it = twice.constBegin(); it != twice.constEnd(); ++it)
        add(out, CheckIssue::Error, *it,
            u8("два столбца с одним названием: сохранится только один"));

    int rxCount = 0;
    bool rxOk = false;
    double rxLat = 0.0, rxLon = 0.0;
    for (int i = 0; i < entries.size(); ++i) {
        if (!entries.at(i).rx)
            continue;
        if (++rxCount == 1)
            rxOk = parseCoords(entries.at(i), &rxLat, &rxLon, out);
    }
    if (rxCount == 0)
        add(out, CheckIssue::Warning, QString(),
            u8("приёмная станция не выбрана: окно задержки ионограмм строится от "
               "координат 0, 0"));
    else if (rxCount > 1)
        add(out, CheckIssue::Warning, QString(),
            u8("приёмными отмечены %1 станции; в chirp_config.py попадёт последняя")
                .arg(rxCount));

    QList<Station> active;
    int activeCount = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const ScheduleEntry &e = entries.at(i);
        if (!e.active)
            continue;
        ++activeCount;

        const char *labels[] = { "период повторения", "chirptime",
                                 "скорость перестройки", "продолжительность",
                                 "частота гетеродина" };
        const QString *texts[] = { &e.rep, &e.chirpt, &e.rate, &e.dur, &e.cf };
        double v[5];
        bool ok = true;
        for (int k = 0; k < 5; ++k) {
            const Number num = parseNumber(*texts[k]);
            if (!num.ok) {
                add(out, CheckIssue::Error, e.name,
                    u8("%1: %2").arg(u8(labels[k]), num.problem));
                ok = false;
            } else if (k != 1 && num.v <= 0.0) {
                add(out, CheckIssue::Error, e.name,
                    k == 0 ? u8("период повторения %1: должен быть больше нуля, иначе "
                                "зондировщик зависнет, выбирая следующий сеанс")
                                 .arg(val(num.v))
                           : u8("%1 %2: должна быть больше нуля")
                                 .arg(u8(labels[k]), val(num.v)));
                ok = false;
            }
            v[k] = num.v;
        }

        double lat = 0.0, lon = 0.0;
        bool coordsOk;
        if (e.rx) {
            coordsOk = rxOk;
            lat = rxLat;
            lon = rxLon;
        } else {
            coordsOk = parseCoords(e, &lat, &lon, out);
        }
        if (!ok)
            continue;

        Station s;
        s.name = e.name;
        s.rep = v[0];
        s.chirpt = v[1];
        s.rateKHz = v[2];
        s.dur = v[3];
        s.cfKHz = v[4];
        checkStation(s, e.fft > 0 ? e.fft : cfg.fft, cfg,
                     coordsOk && rxOk, lat, lon, rxLat, rxLon, out);
        active << s;
    }
    if (activeCount == 0)
        add(out, CheckIssue::Warning, QString(),
            u8("ни одна станция не отмечена для активного расписания: "
               "зондировщику нечего записывать"));

    /* The sounder's list is chirp_config.py's, which is schedule.ini's group
     * order, and a tie goes to the first in it. */
    struct ByName {
        bool operator()(const Station &a, const Station &b) const { return a.name < b.name; }
    };
    std::stable_sort(active.begin(), active.end(), ByName());

    const QVector<int> perDay = active.isEmpty() ? QVector<int>()
                                                 : checkCollisions(active, out);
    if (cfg.checkFiles)
        checkFiles(cfg, active, perDay, out);

    std::stable_sort(out.begin(), out.end(), levelLess);
    return out;
}

QList<ScheduleEntry> readScheduleEntries(QSettings &schedule)
{
    QList<ScheduleEntry> out;
    const QStringList groups = schedule.childGroups();
    for (int i = 0; i < groups.size(); ++i) {
        schedule.beginGroup(groups.at(i));
        ScheduleEntry e;
        e.name   = groups.at(i);
        e.active = schedule.value(QLatin1String("active")).toBool();
        /* configwriter's own test for the receiver. */
        e.rx     = rawValue(schedule.value(QLatin1String("rx")))
                       .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        e.rep    = rawValue(schedule.value(QLatin1String("rep")));
        e.chirpt = rawValue(schedule.value(QLatin1String("chirpt")));
        e.rate   = rawValue(schedule.value(QLatin1String("rate")));
        e.dur    = rawValue(schedule.value(QLatin1String("dur")));
        e.cf     = rawValue(schedule.value(QLatin1String("cf")));
        e.lat    = rawValue(schedule.value(QLatin1String("lat")));
        e.lon    = rawValue(schedule.value(QLatin1String("lon")));
        e.fft    = schedule.value(QLatin1String("fft_count")).toInt();
        schedule.endGroup();
        out << e;
    }
    return out;
}

CheckConfig readCheckConfig(QSettings &config)
{
    CheckConfig c;
    c.sampleRateKHz   = config.value(QLatin1String("sample_rate"), 25000).toDouble();
    c.sampleRateIndex = config.contains(QLatin1String("sample_rate_index"))
                            ? config.value(QLatin1String("sample_rate_index")).toInt() : -1;
    c.dec             = config.value(QLatin1String("dec"), 625).toDouble();
    QBaseSoundParams base;
    base.fftCountIndex = config.value(QLatin1String("fft_count_index"), 5).toInt();
    c.fft             = base.fftCount() > 0 ? base.fftCount() : 16384;
    c.objW            = config.value(QLatin1String("obj_size_horizontal"), 9).toInt();
    c.objH            = config.value(QLatin1String("obj_size_vertical"), 3).toInt();
    c.objLevel        = config.value(QLatin1String("obj_level"), 11.0).toDouble();
    c.dataDir         = config.value(QLatin1String("data_dir")).toString();
    c.soundApp        = config.value(QLatin1String("sound_app")).toString();
    c.keepLfs         = config.value(QLatin1String("keep_lfs"), false).toBool();
    c.keepHours       = config.value(QLatin1String("keep_lfs_hours"), 0.0).toDouble();
    return c;
}

int countIssues(const QList<CheckIssue> &issues, CheckIssue::Level level)
{
    int n = 0;
    for (int i = 0; i < issues.size(); ++i)
        if (issues.at(i).level == level)
            ++n;
    return n;
}

static const char *levelLabel(CheckIssue::Level level)
{
    switch (level) {
    case CheckIssue::Error:   return "Ошибка";
    case CheckIssue::Warning: return "Внимание";
    default:                  return "Справка";
    }
}

QString issueLine(const CheckIssue &issue)
{
    return QString::fromUtf8(levelLabel(issue.level)) + QLatin1String(": ")
           + (issue.station.isEmpty() ? QString() : issue.station + QLatin1String(": "))
           + issue.text;
}

QString issuesHtml(const QList<CheckIssue> &issues)
{
    if (issues.isEmpty())
        return QString::fromUtf8("<span style='color:#2a7a2a'>Замечаний нет.</span>");
    QStringList rows;
    for (int i = 0; i < issues.size(); ++i) {
        const CheckIssue &is = issues.at(i);
        const char *colour = is.level == CheckIssue::Error   ? "#c00000"
                           : is.level == CheckIssue::Warning ? "#a05a00" : "#555555";
        rows << QString(QLatin1String("<span style='color:%1'><b>%2</b></span> %3%4"))
                    .arg(QLatin1String(colour), QString::fromUtf8(levelLabel(is.level)),
                         is.station.isEmpty() ? QString()
                             : QLatin1String("<b>") + is.station.toHtmlEscaped()
                               + QLatin1String("</b>: "),
                         is.text.toHtmlEscaped());
    }
    return rows.join(QLatin1String("<br>"));
}

#include "liveionogram.h"

#include "common.h"
#include "iganalytics.h"
#include "igmath.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

/* python/lfp_products.py's continuous ("snr") scale, from ionograms-handler:
 * the noise floor divided out with 4*ln 2, dB referenced to 1e-3, and a
 * 20..75 dB window. See the comment on IONO_SNR_NOISE_COEF there. */
static const double SNR_NOISE_COEF = 4.0 * 0.69314718055994531;
static const double SNR_DB_REF     = 1e-3;
static const double SNR_VMIN_DB    = 20.0;
static const double SNR_VMAX_DB    = 75.0;

double LiveIonogram::groundDistanceKm(double lat1, double lon1, double lat2, double lon2)
{
    /*
     * python/lfp_products.py's earth_distance_km, not common.cpp's
     * earthDistanceKm. They differ: the console's reproduces the original,
     * whose second term lacks the cos(dlon) factor, and is short wherever the
     * stations are not on one meridian -- 4414 km for Magadan to Yoshkar-Ola
     * against 5476. The sounder's sidecars use the correct one, and it is
     * those that replace this picture, so this draws the same delay window.
     */
    const double d2r = 3.14159265358979323846 / 180.0;
    const double la1 = lat1 * d2r, lo1 = lon1 * d2r, la2 = lat2 * d2r, lo2 = lon2 * d2r;
    const double dlon = lo2 - lo1;
    const double x = std::sin(la1) * std::sin(la2)
                   + std::cos(la1) * std::cos(la2) * std::cos(dlon);
    const double y = std::hypot(std::cos(la2) * std::sin(dlon),
                                std::cos(la1) * std::sin(la2)
                                - std::sin(la1) * std::cos(la2) * std::cos(dlon));
    return 6378.137 * std::atan2(y, x);
}

LiveIonogram::LiveIonogram(const QString &lfsPath, const LiveSettings &settings)
    : m_path(lfsPath),
      m_s(settings),
      m_valid(false),
      m_gone(false),
      m_ifRate(0.0),
      m_expected(0),
      m_available(0),
      m_nfft(0),
      m_cols(0),
      m_rows(0),
      m_ready(0),
      m_freqMin(0.0), m_freqMax(0.0), m_tMin(0.0), m_tMax(0.0),
      m_top(1.0),
      m_buf(0), m_plan(0),
      m_abuf(0), m_aplan(0)
{
    m_s.fftCount = std::max(m_s.fftCount, 16);
    m_s.pad      = std::max(m_s.pad, 1);
    m_s.overlap  = std::max(m_s.overlap, 1);

    FILE *fp = std::fopen(QFile::encodeName(lfsPath).constData(), "rb");
    if (!fp)
        return;
    const bool haveHeader = lfsheader_read(fp, m_h);
    std::fclose(fp);
    if (!haveHeader || m_h.dec == 0 || m_h.rate == 0 || m_h.sample_rate == 0
            || m_h.dur == 0)
        return;

    const int N = m_s.fftCount;
    m_ifRate   = (double)m_h.sample_rate / (double)m_h.dec;
    m_expected = (qint64)(m_ifRate * (double)m_h.dur);
    const int specCount = (int)(m_expected / N);
    if (specCount <= 0)
        return;
    m_cols = specCount * m_s.overlap;

    m_freqMin = ((double)m_h.cf - (double)m_h.sample_rate / 2.0) / HZ_IN_MHZ;
    m_freqMax = m_freqMin + (double)m_h.dur * (double)m_h.rate / HZ_IN_MHZ;

    const double ground = groundDistanceKm(m_h.tx_latitude, m_h.tx_longitude,
                                           m_h.rx_latitude, m_h.rx_longitude);
    const double rminKm = rayDistanceKm(ground) - VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;
    const double rmaxKm = rminKm + VIRT_HEIGHT_WINDOW_KM_DEFAULT
                                 + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;

    /* km of group path per Hz of beat, and the edge of the unambiguous range. */
    const double scale = LIGHT_SPEED_KM_S / (double)m_h.rate;
    const double hMax  = scale * m_ifRate / 2.0;

    int lo, hi;
    if (m_s.pad > 1 || m_s.overlap > 1) {
        /* display_ionogram: the FFT's own axis, row r at -hMax + (r+1)*step. */
        m_nfft = N * m_s.pad;
        const double step = scale * m_ifRate / m_nfft;
        lo = std::max(0, (int)std::floor((rminKm + hMax) / step - 1.0));
        hi = std::min(m_nfft - 1, (int)std::ceil((rmaxKm + hMax) / step - 1.0));
        m_tMin = timeMsFromHeightKm(-hMax + (lo + 1.0) * step);
        m_tMax = timeMsFromHeightKm(-hMax + (hi + 1.0) * step);
    } else {
        /* geometry(), and loadLfsData: the original's (N - 1) spacing. */
        m_nfft = N;
        const double kmPerRow = 2.0 * hMax / (double)(N - 1);
        lo = std::max(0, (int)std::floor((rminKm + hMax) / kmPerRow));
        hi = std::min(N - 1, (int)std::ceil((rmaxKm + hMax) / kmPerRow));
        m_tMin = timeMsFromHeightKm(-hMax + lo * kmPerRow);
        m_tMax = timeMsFromHeightKm(-hMax + hi * kmPerRow);
    }
    m_rows = hi - lo + 1;
    if (m_rows <= 1)
        return;

    /* Picture row r is row lo + r of the fftshifted, reversed spectrum, which
     * is this bin of the unshifted one -- the gather display_spectra does. */
    m_gather.resize(m_rows);
    for (int r = 0; r < m_rows; ++r)
        m_gather[r] = (m_nfft - 1 - (lo + r) + m_nfft / 2) % m_nfft;

    m_window = calculateHanningWindow((unsigned)N);

    m_buf  = fftw_alloc_complex((size_t)m_nfft);
    m_plan = fftw_plan_dft_1d(m_nfft, m_buf, m_buf, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!m_s.snrMode) {
        m_abuf  = fftw_alloc_complex((size_t)N);
        m_aplan = fftw_plan_dft_1d(N, m_abuf, m_abuf, FFTW_FORWARD, FFTW_ESTIMATE);
        m_limits.assign(specCount, 0.0f);
        m_limitDone.assign(specCount, 0);
        m_raw.resize(m_cols);
    }
    m_out.resize(m_cols);
    m_valid = (m_buf && m_plan && (m_s.snrMode || (m_abuf && m_aplan)));
}

LiveIonogram::~LiveIonogram()
{
    if (m_plan)  fftw_destroy_plan(m_plan);
    if (m_aplan) fftw_destroy_plan(m_aplan);
    if (m_buf)   fftw_free(m_buf);
    if (m_abuf)  fftw_free(m_abuf);
}

QRectF LiveIonogram::area() const
{
    return QRectF(m_freqMin, m_tMin, m_freqMax - m_freqMin, m_tMax - m_tMin);
}

double LiveIonogram::frontierMHz() const
{
    if (m_cols <= 0)
        return m_freqMin;
    return m_freqMin + (m_freqMax - m_freqMin) * (double)m_ready / (double)m_cols;
}

double LiveIonogram::bottomDb() const
{
    return m_s.snrMode ? SNR_VMIN_DB : 0.0;
}

double LiveIonogram::topDb() const
{
    return m_s.snrMode ? SNR_VMAX_DB : m_top;
}

QDateTime LiveIonogram::start() const
{
    return QDateTime(QDate(m_h.start_year, m_h.start_month, m_h.start_day),
                     QTime(m_h.start_hour, m_h.start_minute, m_h.start_second),
                     Qt::UTC);
}

qint64 LiveIonogram::needed(int j) const
{
    const int N = m_s.fftCount;
    const double hop = (double)N / (double)m_s.overlap;
    /* Window j is centred (j + 0.5) columns into the band, as the console
     * draws it; with overlap 1 these are exactly the analysis spectra. */
    qint64 end = (qint64)std::llround((j + 0.5) * hop - N / 2.0) + N;
    if (!m_s.snrMode) {
        /* The gate threshold comes from the whole analysis spectrum. */
        const qint64 spectrumEnd = (qint64)(j / m_s.overlap + 1) * N;
        end = std::max(end, spectrumEnd);
    }
    return std::min(end, m_expected);
}

bool LiveIonogram::readSamples(FILE *fp, qint64 from, int count,
                               std::vector<float> &iq) const
{
    iq.assign((size_t)count * 2, 0.0f);
    if (count <= 0)
        return true;
    if (std::fseek(fp, (long)(sizeof(lfs_header) + from * 8), SEEK_SET) != 0)
        return false;
    return std::fread(iq.data(), 8, (size_t)count, fp) == (size_t)count;
}

float LiveIonogram::analysisLimit(FILE *fp, int c)
{
    if (c >= (int)m_limits.size())
        c = (int)m_limits.size() - 1;
    if (m_limitDone[c])
        return m_limits[c];

    /* As build_spectra and loadLfsData: window, FFT, power, normalise by the
     * median, and the Rosin threshold of the whole spectrum in dB. */
    const int N = m_s.fftCount;
    const qint64 first = (qint64)c * N;
    const int count = (int)std::max<qint64>(0, std::min<qint64>(N, m_available - first));
    std::vector<float> iq;
    readSamples(fp, first, count, iq);
    for (int k = 0; k < N; ++k) {
        const float re = k < count ? iq[2 * k] : 0.0f;
        const float im = k < count ? iq[2 * k + 1] : 0.0f;
        m_abuf[k][0] = (double)(m_window[k] * re);
        m_abuf[k][1] = (double)(m_window[k] * im);
    }
    fftw_execute(m_aplan);

    std::vector<double> power(N);
    for (int k = 0; k < N; ++k)
        power[k] = m_abuf[k][0] * m_abuf[k][0] + m_abuf[k][1] * m_abuf[k][1];
    const double median = getMedian(power.data(), N);

    std::vector<float> db(N);
    for (int k = 0; k < N; ++k) {
        const double p = median > 0.0 ? power[k] / median : power[k];
        db[k] = (float)(10.0 * std::log10(p > 0.0 ? p : 1e-300));
    }
    m_limits[c] = getPowerDynamicLimit(db.data(), N);
    m_limitDone[c] = 1;
    return m_limits[c];
}

void LiveIonogram::computeColumn(FILE *fp, int j)
{
    const int N = m_s.fftCount;
    const double hop = (double)N / (double)m_s.overlap;
    const qint64 start = (qint64)std::llround((j + 0.5) * hop - N / 2.0);

    /* The window, zero-filled where it reaches past either end of the
     * recording, then zero-padded to nfft. */
    const qint64 a = std::max<qint64>(start, 0);
    const qint64 b = std::min<qint64>(start + N, m_available);
    std::vector<float> iq;
    readSamples(fp, a, (int)std::max<qint64>(0, b - a), iq);

    for (int k = 0; k < m_nfft; ++k)
        m_buf[k][0] = m_buf[k][1] = 0.0;
    for (qint64 s = a; s < b; ++s) {
        const int k = (int)(s - start);
        const size_t i = (size_t)(s - a) * 2;
        m_buf[k][0] = (double)(m_window[k] * iq[i]);
        m_buf[k][1] = (double)(m_window[k] * iq[i + 1]);
    }
    fftw_execute(m_plan);

    /* Normalised by the median of the unpadded bins -- every pad-th bin of a
     * zero-padded FFT is one -- so values mean what the analysis ones do. */
    std::vector<double> native(N);
    for (int k = 0; k < N; ++k) {
        const fftw_complex &v = m_buf[(size_t)k * m_s.pad];
        native[k] = v[0] * v[0] + v[1] * v[1];
    }
    const double median = getMedian(native.data(), N);

    std::vector<float> &col = m_s.snrMode ? m_out[j] : m_raw[j];
    col.assign(m_rows, 0.0f);

    if (m_s.snrMode) {
        for (int r = 0; r < m_rows; ++r) {
            const fftw_complex &v = m_buf[m_gather[r]];
            double p = v[0] * v[0] + v[1] * v[1];
            if (median > 0.0)
                p /= median;
            const double rel = std::max(p / SNR_NOISE_COEF, SNR_DB_REF);
            col[r] = (float)(10.0 * std::log10(rel / SNR_DB_REF));
        }
        return;
    }

    const float limit = analysisLimit(fp, j / m_s.overlap);
    float peak = 0.0f;
    for (int r = 0; r < m_rows; ++r) {
        const fftw_complex &v = m_buf[m_gather[r]];
        double p = v[0] * v[0] + v[1] * v[1];
        if (median > 0.0)
            p /= median;
        const float db = (float)(10.0 * std::log10(p > 0.0 ? p : 1e-300));
        col[r] = (limit > db) ? 0.0f : db;
        peak = std::max(peak, col[r]);
    }
    /* The second, statistical gate -- per spectrum, so it can go here. */
    if (peak > 0.0f) {
        const float noise = (float)noiseLevelDbForNormalisedSpectra();
        for (int r = 0; r < m_rows; ++r)
            if (col[r] < noise)
                col[r] = 0.0f;
    }
}

void LiveIonogram::despeckle(int from, int to)
{
    /*
     * The speckle filter counts neighbours within halfW columns either side,
     * so new columns [from, to) change the result for [from - halfW, to), and
     * computing those needs the sources from from - 2*halfW on. Run on that
     * band alone, the filter gives exactly what it would on everything
     * received so far, since its edge clamp then falls where the full array's
     * does. The newest halfW columns are eroded by that clamp and fill in on
     * the next update; when the capture is complete this is the whole-array
     * result.
     */
    const int W = (int)m_s.objW * m_s.overlap;
    const int H = (int)m_s.objH * m_s.pad;
    const float level = m_s.objLevel * (float)(m_s.overlap * m_s.pad);
    const int halfW = W / 2;

    const int s  = std::max(0, from - 2 * halfW);
    const int x0 = std::max(0, from - halfW);
    const int n  = to - s;

    std::vector<const float *> src(n);
    std::vector<std::vector<float> > tmp(n, std::vector<float>(m_rows, 0.0f));
    std::vector<float *> dst(n);
    for (int i = 0; i < n; ++i) {
        src[i] = m_raw[s + i].data();
        dst[i] = tmp[i].data();
    }
    deleteSmallObjects(dst.data(), src.data(), n, m_rows, (unsigned)W, (unsigned)H, level);
    for (int x = x0; x < to; ++x)
        m_out[x].swap(tmp[x - s]);

    /* applyProducts scales a gated picture to its own maximum. */
    float top = 0.0f;
    for (int x = 0; x < to; ++x)
        for (int r = 0; r < m_rows; ++r)
            top = std::max(top, m_out[x][r]);
    m_top = top > 0.0f ? top : 1.0;
}

bool LiveIonogram::update(int maxColumns)
{
    if (!m_valid || m_gone || m_ready >= m_cols)
        return false;

    FILE *fp = std::fopen(QFile::encodeName(m_path).constData(), "rb");
    if (!fp) {
        /* Renamed to .partial (stopped early) or already archived and
         * deleted before the last columns were read. */
        m_gone = true;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    m_available = std::max<qint64>(0, ((qint64)size - (qint64)sizeof(lfs_header)) / 8);

    const int first = m_ready;
    int j = m_ready;
    while (j < m_cols && j - first < maxColumns && needed(j) <= m_available) {
        computeColumn(fp, j);
        ++j;
    }
    std::fclose(fp);

    if (j == first)
        return false;
    m_ready = j;
    if (!m_s.snrMode)
        despeckle(first, j);
    return true;
}

#ifndef LIVEIONOGRAM_H
#define LIVEIONOGRAM_H

#include "lfs_header.h"

#include <QDateTime>
#include <QRectF>
#include <QString>

#include <fftw3.h>

#include <vector>

/*
 * A capture that is still being recorded, drawn as it arrives.
 *
 * The original did this: QRxIonogram's timerEvent read whatever the sounder had
 * appended since the last tick (readNewLfsSpecData) and the ionogram grew from
 * the left across the sounding. This port only ever loaded finished captures,
 * so for 250 s the current panel showed the previous sounding and then changed
 * all at once.
 *
 * Each update() reads the samples appended since the last one and computes
 * only the columns they complete. The arithmetic is the sidecar builder's
 * (python/lfp_products.py, compute and display_ionogram), on the same grid --
 * FFT length, zero-padding, overlap and mode -- so when the sounder's sidecar
 * replaces the live picture at the end of the sounding the picture does not
 * jump. Two things differ, both only while recording:
 *
 *   - in gated mode the speckle filter needs neighbours on both sides, so the
 *     newest half-window of columns stays empty until the next ones arrive;
 *   - LUF, MUF, SNR and PDP are not computed. The variation panels get them
 *     from the finished capture, once.
 *
 * Columns not yet received are NaN, which the colour map draws transparent:
 * the panel's own background shows where the sweep has not reached.
 */
struct LiveSettings
{
    int      fftCount;
    int      pad;          /* iono_pad: picture rows per FFT bin */
    int      overlap;      /* iono_overlap: picture columns per spectrum */
    bool     snrMode;      /* iono_mode = snr: continuous, nothing cut */
    unsigned objW;         /* speckle window, analysis spectra */
    unsigned objH;         /* speckle window, analysis delay rows */
    float    objLevel;

    LiveSettings()
        : fftCount(16384), pad(1), overlap(1), snrMode(false),
          objW(9), objH(3), objLevel(11.0f) {}
};

class LiveIonogram
{
public:
    LiveIonogram(const QString &lfsPath, const LiveSettings &settings);
    ~LiveIonogram();

    /* False when the header could not be read yet, or describes no sweep. */
    bool isValid() const { return m_valid; }
    QString path() const { return m_path; }
    const LiveSettings &settings() const { return m_s; }

    /*
     * Take in whatever the sounder has written since the last call. At most
     * `maxColumns` new columns per call, so that opening the console in the
     * middle of a sounding catches up over a few ticks rather than in one long
     * stall beside a dechirp that needs the CPU. Returns true when anything
     * on screen changed. False also once the file has gone -- a sounding that
     * stopped early is renamed to .partial -- which gone() then reports.
     */
    bool update(int maxColumns = 96);
    bool gone() const { return m_gone; }

    int  columns() const { return m_cols; }
    int  rows() const    { return m_rows; }
    int  ready() const   { return m_ready; }
    bool complete() const { return m_ready >= m_cols; }

    /* Column c, rows ascending in delay. Only c < ready() is meaningful. */
    const float *column(int c) const { return m_out[c].data(); }

    /* Frequency (x, MHz) by delay (y, ms), as the finished picture has it. */
    QRectF area() const;
    /* The sweep frequency the received columns have reached. */
    double frontierMHz() const;

    /* Colour scale: the bottom of the raster's range and its top, as
     * QRxIonogram::applyProducts derives them for the finished capture. */
    double bottomDb() const;
    double topDb() const;

    QDateTime start() const;
    const lfs_header &header() const { return m_h; }

    /* Great-circle distance, as the sidecar builder computes it. */
    static double groundDistanceKm(double lat1, double lon1, double lat2, double lon2);

private:
    bool readSamples(FILE *fp, qint64 from, int count, std::vector<float> &iq) const;
    /* Rosin threshold of analysis spectrum c, computed on first use. */
    float analysisLimit(FILE *fp, int c);
    void computeColumn(FILE *fp, int j);
    /* Re-run the speckle filter over the columns new samples can affect. */
    void despeckle(int from, int to);
    /* Samples column j needs before it can be drawn. */
    qint64 needed(int j) const;

    QString      m_path;
    LiveSettings m_s;
    lfs_header   m_h;
    bool         m_valid;
    bool         m_gone;

    double m_ifRate;
    qint64 m_expected;     /* samples in a complete capture */
    qint64 m_available;    /* samples on disk at the last update */
    int    m_nfft;
    int    m_cols;
    int    m_rows;
    int    m_ready;
    double m_freqMin, m_freqMax, m_tMin, m_tMax;
    double m_top;          /* gated: the largest value drawn so far */

    std::vector<int>   m_gather;   /* picture row -> unshifted FFT bin */
    std::vector<float> m_window;

    /* Gated mode keeps the picture before the speckle filter as well, since
     * the filter is re-run as neighbours arrive. */
    std::vector<std::vector<float> > m_raw;
    std::vector<std::vector<float> > m_out;
    std::vector<float> m_limits;
    std::vector<char>  m_limitDone;

    fftw_complex *m_buf;     /* nfft points: the picture's spectra */
    fftw_plan     m_plan;
    fftw_complex *m_abuf;    /* fftCount points: the analysis spectra */
    fftw_plan     m_aplan;
};

#endif /* LIVEIONOGRAM_H */

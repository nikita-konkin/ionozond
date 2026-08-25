/*
 * Render an ionogram straight from a .lfs capture, headless, to a PNG.
 *
 * This exercises the whole recovered chain end to end: header -> spectra ->
 * median normalisation -> dB -> display window -> colour map. The output can be
 * compared against the ionograms in doc/user_manual.pdf and against the
 * original binary's own display.
 *
 *   render_ionogram <file.lfs> <out.png> [fftCount] [colormapIndex]
 */
#include "../src/common.h"
#include "../src/igmath.h"
#include "../src/lfs_header.h"
#include "../src/qigcolormap.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <functional>
#include <utility>
#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <file.lfs> <out.png> [fftCount] [colormapIndex]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *out = argv[2];
    const int fftCount = argc > 3 ? std::atoi(argv[3]) : 16384;
    const int cmapIndex = argc > 4 ? std::atoi(argv[4]) : 1;

    FILE *fp = std::fopen(path, "rb");
    if (!fp) { std::perror("fopen"); return 1; }

    lfs_header h;
    if (!lfsheader_read(fp, h)) { std::fprintf(stderr, "not a valid .lfs\n"); return 1; }

    std::fseek(fp, 0, SEEK_END);
    const long fileSize = std::ftell(fp);
    std::fseek(fp, sizeof(lfs_header), SEEK_SET);

    const long totalSamples = (fileSize - (long)sizeof(lfs_header)) / 8;
    const int nSpec = (int)(totalSamples / fftCount);

    const double ifRate = (double)h.sample_rate / (double)h.dec;
    std::printf("%s -> %s   %04u-%02u-%02u %02u:%02u:%02u\n", h.tx_name, h.rx_name,
                h.start_year, h.start_month, h.start_day,
                h.start_hour, h.start_minute, h.start_second);
    std::printf("spectra %d x %d   if_rate %.0f Hz\n", nSpec, fftCount, ifRate);

    /* ---- build every spectrum ---- */
    std::vector<std::vector<double> > storage(nSpec, std::vector<double>(fftCount));
    std::vector<double *> spec(nSpec);
    for (int i = 0; i < nSpec; ++i) spec[i] = storage[i].data();

    std::vector<float> window = calculateHanningWindow((unsigned)fftCount);
    double maxValue = 0.0;
    const int built = buildSpectra(fp, fftCount, window.data(), nSpec, spec.data(), &maxValue);
    std::fclose(fp);
    if (built <= 0) { std::fprintf(stderr, "no spectra\n"); return 1; }
    std::printf("built %d spectra, max_value %.6g (%.2f dB)\n",
                built, maxValue, 10.0 * std::log10(maxValue));

    /* ---- axes ---- */
    const double freqStartMHz = ((double)h.cf - (double)h.sample_rate / 2.0) / HZ_IN_MHZ;
    const double freqStopMHz  = freqStartMHz + (double)h.dur * (double)h.rate / HZ_IN_MHZ;

    const double dKm    = earthDistanceKm(h.tx_latitude, h.tx_longitude,
                                          h.rx_latitude, h.rx_longitude);
    const double rayKm  = rayDistanceKm(dKm);
    const double rminKm = rayKm - VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;
    const double rmaxKm = rminKm + VIRT_HEIGHT_WINDOW_KM_DEFAULT
                                 + VIRT_HEIGHT_WINDOW_MARGIN_KM_DEFAULT;

    /*
     * Row -> virtual height.
     *
     * After buildSpectra()'s fftshift and reverse the row index ASCENDS with
     * delay: row 0 is the most negative height, row n-1 the most positive.
     * Determined empirically -- the returned-signal energy for this capture
     * sits at rows 8554..8575, which this mapping places at +2655..+2809 km,
     * just above the 2550 km ray path. The opposite mapping put it at
     * -2684 km, which is unphysical.
     */
    const double hMax = LIGHT_SPEED_KM_S * (ifRate / 2.0) / (double)h.rate;
    const double hMin = -hMax;
    const double kmPerRow = (hMax - hMin) / (double)(fftCount - 1);

    const int rowLow  = (int)std::floor((rminKm - hMin) / kmPerRow);
    const int rowHigh = (int)std::ceil((rmaxKm - hMin) / kmPerRow);
    const int rows = rowHigh - rowLow + 1;
    const int rowTop = rowLow, rowBot = rowHigh;   /* for the scans below */

    std::printf("freq  %.3f .. %.3f MHz\n", freqStartMHz, freqStopMHz);
    std::printf("range %.1f .. %.1f km  (t %.3f .. %.3f ms)\n",
                rminKm, rmaxKm, timeMsFromHeightKm(rminKm), timeMsFromHeightKm(rmaxKm));
    std::printf("rows  %d .. %d of %d  (%d rows)\n", rowTop, rowBot, fftCount, rows);
    if (rowTop < 0 || rowBot >= fftCount || rows <= 0) {
        std::fprintf(stderr, "window falls outside the data\n");
        return 1;
    }

    /* ---- colour scale ---- */
    std::vector<const double *> cspec(built);
    for (int i = 0; i < built; ++i) cspec[i] = spec[i];
    const double floor5 = percentileFloor(cspec.data(), built, fftCount, 5.0);
    const double maxDb = 10.0 * std::log10(maxValue);

    /*
     * The frame-wide maximum is normally the direct signal, which lies well
     * outside the delay window, so scaling to it flattens everything visible.
     * Scale to the window's own maximum instead; RENDER_GLOBAL_SCALE=1
     * restores the frame-wide behaviour for comparison.
     */
    double winMax = 0.0;
    long nanCount = 0, infCount = 0;
    for (int r = rowTop; r <= rowBot; ++r) {
        for (int c = 0; c < built; ++c) {
            const double p = spec[c][r];
            if (p != p) { ++nanCount; continue; }
            if (!std::isfinite(p)) { ++infCount; continue; }
            if (p > winMax) winMax = p;
        }
    }
    const double winMaxDb = 10.0 * std::log10(winMax > 0.0 ? winMax : 1.0);
    const bool globalScale = (qgetenv("RENDER_GLOBAL_SCALE") == "1");
    const double topDb = globalScale ? maxDb : winMaxDb;

    const QwtInterval interval(0.125 * topDb, topDb);
    std::printf("frame max %.3f dB, window max %.3f dB, 5%% floor %.3f dB\n",
                maxDb, winMaxDb, 10.0 * std::log10(floor5));
    std::printf("non-finite in window: %ld NaN, %ld inf\n", nanCount, infCount);
    std::printf("colour interval %.3f .. %.3f dB (%s)\n",
                interval.minValue(), interval.maxValue(),
                globalScale ? "frame max" : "window max");

    /*
     * Diagnostic: where does the energy actually sit in delay? Averaging each
     * row over all spectra shows the direct signal and any ionospheric trace,
     * and confirms whether the row -> height mapping is the right way up.
     */
    if (qgetenv("RENDER_PROFILE") == "1") {
        std::vector<std::pair<double, int> > rowMean(fftCount);
        for (int r = 0; r < fftCount; ++r) {
            double sum = 0.0;
            for (int c = 0; c < built; ++c) sum += spec[c][r];
            rowMean[r] = std::make_pair(sum / built, r);
        }
        std::vector<std::pair<double, int> > top = rowMean;
        std::partial_sort(top.begin(), top.begin() + 15, top.end(),
                          std::greater<std::pair<double, int> >());
        std::printf("\nstrongest delay rows (mean power over all spectra):\n");
        for (int i = 0; i < 15; ++i) {
            const int r = top[i].second;
            const double km = hMin + r * kmPerRow;
            std::printf("  row %6d  %10.1f km  %9.3f ms  mean %.4f (%.2f dB)\n",
                        r, km, timeMsFromHeightKm(km), top[i].first,
                        10.0 * std::log10(top[i].first));
        }
        std::printf("\n");
    }

    QIgColorMap cmap(colorLevelsForIndex(cmapIndex));

    /* ---- raster ---- */
    QImage img(built, rows, QImage::Format_RGB32);
    double peak = -1e300; int peakRow = 0, peakCol = 0;
    for (int r = 0; r < rows; ++r) {
        /* top of the image is the largest delay, matching the original's
         * y axis which runs 8 ms at the bottom to 13.5 ms at the top */
        const int dataRow = rowHigh - r;
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(r));
        for (int c = 0; c < built; ++c) {
            const double p = spec[c][dataRow];
            const double db = 10.0 * std::log10(p > 0.0 ? p : 1e-300);
            if (db > peak) { peak = db; peakRow = r; peakCol = c; }
            line[c] = cmap.rgb(interval, db);
        }
    }
    std::printf("peak %.2f dB at row %d (%.1f km, %.3f ms), col %d (%.3f MHz)\n",
                peak, peakRow,
                hMin + (rowHigh - peakRow) * kmPerRow,
                timeMsFromHeightKm(hMin + (rowHigh - peakRow) * kmPerRow),
                peakCol,
                freqStartMHz + (freqStopMHz - freqStartMHz) * peakCol / (built - 1));

    /* Scale up so the thin delay window is legible. */
    QImage scaled = img.scaled(built, rows * 3, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (!scaled.save(QString::fromLocal8Bit(out))) {
        std::fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", out, scaled.width(), scaled.height());
    return 0;
}

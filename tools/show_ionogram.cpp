/*
 * Build a QRxIonogram widget, load a capture into it, and grab it to a PNG.
 * Runs offscreen, so it needs no X server.
 *
 *   show_ionogram <file.lfs> <out.png> [width] [height]
 */
#include "../src/common.h"
#include "../src/qrxionogram.h"

#include <QApplication>
#include <QPixmap>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <file.lfs> <out.png> [w] [h]\n", argv[0]);
        return 2;
    }
    const QString capture = QString::fromLocal8Bit(argv[1]);
    const QString out = QString::fromLocal8Bit(argv[2]);
    const int w = argc > 3 ? std::atoi(argv[3]) : 700;
    const int h = argc > 4 ? std::atoi(argv[4]) : 380;

    /* Settings as the shipped config has them. */
    QBaseSoundParams base;
    base.fftCountIndex = 5;          /* 16384 */
    base.sampleRateIndex = 3;        /* 25000 kHz */
    base.igColormapIndex = 1;        /* WHITE_BASE_COLORS, as shipped */
    base.colormapGradient = true;
    base.igVerticalScaleIndex = 1;   /* t, ms */

    QTxParams tx;
    QRxParams rx;

    QRxIonogram plot(base, tx, rx);
    plot.resize(w, h);

    if (!plot.loadLfsData(capture)) {
        std::fprintf(stderr, "failed to load %s\n", argv[1]);
        return 1;
    }
    std::printf("loaded %s\n", argv[1]);
    std::printf("  freq   %.3f .. %.3f MHz\n",
                plot.horizontalInterval().left(), plot.horizontalInterval().right());
    std::printf("  delay  %.3f .. %.3f ms\n",
                plot.verticalInterval().top(), plot.verticalInterval().bottom());

    plot.show();
    app.processEvents();

    QPixmap pm = plot.grab();
    if (!pm.save(out)) {
        std::fprintf(stderr, "could not write %s\n", argv[2]);
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", argv[2], pm.width(), pm.height());
    return 0;
}

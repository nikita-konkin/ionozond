/*
 * Verification 2: the reconstructed spectrum pipeline.
 *
 * Reads the first N spectra out of a real .lfs capture, runs the reconstructed
 * pipeline, prints a summary and dumps the result for python/spectrum_oracle.py
 * to check against an independent NumPy implementation.
 *
 *   test_spectrum <file.lfs> <specPointCount> <nSpec> <out.bin>
 */
#include "../src/lfs_header.h"
#include "../src/igmath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <file.lfs> <specPointCount> <nSpec> <out.bin>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const int specPointCount = std::atoi(argv[2]);
    const int nSpec = std::atoi(argv[3]);
    const char *outPath = argv[4];

    FILE *fp = std::fopen(path, "rb");
    if (!fp) { std::perror("fopen"); return 1; }

    lfs_header h;
    if (!lfsheader_read(fp, h)) {
        std::fprintf(stderr, "not a valid .lfs file\n");
        return 1;
    }
    std::printf("capture   %s  %04u-%02u-%02u %02u:%02u:%02u\n", h.tx_name,
                h.start_year, h.start_month, h.start_day,
                h.start_hour, h.start_minute, h.start_second);
    std::printf("params    specPointCount=%d nSpec=%d  if_rate=%.0f Hz\n",
                specPointCount, nSpec, (double)h.sample_rate / h.dec);

    std::vector<float> window = calculateHanningWindow((unsigned)specPointCount);
    double wsum = 0.0;
    for (int i = 0; i < specPointCount; ++i) wsum += window[i];
    std::printf("window    n=%d  sum=%.9f  w[0]=%.9g w[1]=%.9g w[n/2]=%.9f\n",
                specPointCount, wsum, window[0], window[1], window[specPointCount / 2]);

    /* allocate the spectra */
    std::vector<std::vector<double> > storage(nSpec, std::vector<double>(specPointCount));
    std::vector<double *> specPower(nSpec);
    for (int i = 0; i < nSpec; ++i) specPower[i] = storage[i].data();

    double maxValue = 0.0;
    int built = buildSpectra(fp, specPointCount, window.data(), nSpec, specPower.data(), &maxValue);
    std::fclose(fp);

    std::printf("built     %d spectra, max_value = %.9g\n", built, maxValue);
    if (built <= 0) return 1;

    std::vector<const double *> cspec(built);
    for (int i = 0; i < built; ++i) cspec[i] = specPower[i];
    double floor5 = percentileFloor(cspec.data(), built, specPointCount, 5.0);
    std::printf("floor     5th percentile = %.9g  (%.6f dB)\n", floor5, 10.0 * std::log10(floor5));
    std::printf("maxDb     %.6f dB  -> level interval [%.6f, %.6f]\n",
                10.0 * std::log10(maxValue), 0.125 * 10.0 * std::log10(maxValue), 10.0 * std::log10(maxValue));

    for (int s = 0; s < built && s < 3; ++s) {
        std::printf("spec[%d]   [0]=%.9g [1]=%.9g [n/2]=%.9g [n-1]=%.9g\n", s,
                    specPower[s][0], specPower[s][1],
                    specPower[s][specPointCount / 2], specPower[s][specPointCount - 1]);
    }

    FILE *out = std::fopen(outPath, "wb");
    if (!out) { std::perror("fopen out"); return 1; }
    for (int s = 0; s < built; ++s)
        std::fwrite(specPower[s], sizeof(double), (size_t)specPointCount, out);
    std::fclose(out);
    std::printf("wrote     %s (%d x %d doubles)\n", outPath, built, specPointCount);
    return 0;
}

/*
 * Verification 1: the reconstructed lfs_header must have the exact binary
 * layout of the original, and must parse real captures correctly.
 *
 * Build/run: see tests/run_tests.sh
 */
#include "../src/lfs_header.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* Offsets decoded from the binary and confirmed against real files. */
static void test_layout()
{
  std::printf("layout:\n");
  CHECK(sizeof(lfs_header) == 512);
  CHECK(offsetof(lfs_header, format)          == 0x000);
  CHECK(offsetof(lfs_header, format_ver)      == 0x004);
  CHECK(offsetof(lfs_header, header_id)       == 0x008);
  CHECK(offsetof(lfs_header, header_size)     == 0x00C);
  CHECK(offsetof(lfs_header, tx_name)         == 0x00E);
  CHECK(offsetof(lfs_header, tx_latitude)     == 0x04E);
  CHECK(offsetof(lfs_header, tx_longitude)    == 0x052);
  CHECK(offsetof(lfs_header, rx_name)         == 0x056);
  CHECK(offsetof(lfs_header, rx_latitude)     == 0x096);
  CHECK(offsetof(lfs_header, rx_longitude)    == 0x09A);
  CHECK(offsetof(lfs_header, start_year)      == 0x09E);
  CHECK(offsetof(lfs_header, start_daynumber) == 0x0A0);
  CHECK(offsetof(lfs_header, start_month)     == 0x0A2);
  CHECK(offsetof(lfs_header, start_day)       == 0x0A4);
  CHECK(offsetof(lfs_header, start_hour)      == 0x0A6);
  CHECK(offsetof(lfs_header, start_minute)    == 0x0A8);
  CHECK(offsetof(lfs_header, start_second)    == 0x0AA);
  CHECK(offsetof(lfs_header, start_epoch)     == 0x0AC);
  CHECK(offsetof(lfs_header, chirpt)          == 0x0B0);
  CHECK(offsetof(lfs_header, cf)              == 0x0B4);
  CHECK(offsetof(lfs_header, dur)             == 0x0B8);
  CHECK(offsetof(lfs_header, rate)            == 0x0BA);
  CHECK(offsetof(lfs_header, rep)             == 0x0BE);
  CHECK(offsetof(lfs_header, rmin)            == 0x0C2);
  CHECK(offsetof(lfs_header, rmax)            == 0x0C6);
  CHECK(offsetof(lfs_header, dec)             == 0x0CA);
  CHECK(offsetof(lfs_header, sample_rate)     == 0x0CE);
  CHECK(offsetof(lfs_header, whiten)          == 0x0D2);
  CHECK(offsetof(lfs_header, whiten_len)      == 0x0D4);
  CHECK(offsetof(lfs_header, whiten_n)        == 0x0D8);
  CHECK(offsetof(lfs_header, reserved)        == 0x0DC);
  std::printf("  sizeof(lfs_header) = %zu\n", sizeof(lfs_header));
}

static void dump(const char *path)
{
  std::printf("\n%s:\n", path);
  FILE *f = std::fopen(path, "rb");
  if (!f) { std::printf("  FAIL  cannot open\n"); ++failures; return; }

  lfs_header h;
  bool ok = lfsheader_read(f, h);
  if (!ok) { std::printf("  FAIL  lfsheader_read rejected the file\n"); ++failures; std::fclose(f); return; }

  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fclose(f);

  long samples = (size - (long)sizeof(lfs_header)) / 8;
  double if_rate = (double)h.sample_rate / (double)h.dec;

  std::printf("  format          %.4s  ver %.1f  id '%.4s'  header_size %u\n",
              h.format, h.format_ver, h.header_id, h.header_size);
  std::printf("  tx              %s  %.2f / %.2f\n", h.tx_name, h.tx_latitude, h.tx_longitude);
  std::printf("  rx              %s  %.2f / %.2f\n", h.rx_name, h.rx_latitude, h.rx_longitude);
  std::printf("  start           %04u-%02u-%02u %02u:%02u:%02u  (yday %u, epoch %u)\n",
              h.start_year, h.start_month, h.start_day,
              h.start_hour, h.start_minute, h.start_second,
              h.start_daynumber, h.start_epoch);
  std::printf("  chirpt %u  cf %u Hz  dur %u s  rate %u Hz/s  rep %u s\n",
              h.chirpt, h.cf, h.dur, h.rate, h.rep);
  std::printf("  rmin %d  rmax %d  dec %u  sample_rate %u Hz  -> if_rate %.0f Hz\n",
              h.rmin, h.rmax, h.dec, h.sample_rate, if_rate);
  std::printf("  whiten %u  whiten_len %u  whiten_n %u\n",
              h.whiten, h.whiten_len, h.whiten_n);
  std::printf("  file %ld bytes -> %ld complex64 samples = %.2f s at if_rate\n",
              size, samples, samples / if_rate);

  /* The capture must be a whole number of samples after the header. */
  CHECK((size - (long)sizeof(lfs_header)) % 8 == 0);
  /* Recorded duration should agree with the advertised session length. */
  double secs = samples / if_rate;
  CHECK(secs > 0.0 && secs <= (double)h.dur + 1.0);
}

/* A header with the newer format_ver 1.1 / size 512 must be rejected. */
static void test_rejects_v11()
{
  std::printf("\nrejects format_ver 1.1:\n");
  const char *tmp = "/tmp/bad_ver.lfs";
  lfs_header h;
  h.format_ver  = 1.1f;
  h.header_size = 512;
  FILE *f = std::fopen(tmp, "wb");
  std::fwrite(&h, 1, sizeof(h), f);
  /* one sample of payload so feof() is not hit */
  double pad = 0; std::fwrite(&pad, 1, 8, f);
  std::fclose(f);

  f = std::fopen(tmp, "rb");
  lfs_header out;
  bool ok = lfsheader_read(f, out);
  std::fclose(f);
  CHECK(ok == false);
  if (!ok) std::printf("  ok, rejected as expected\n");
}

int main(int argc, char **argv)
{
  test_layout();
  test_rejects_v11();
  for (int i = 1; i < argc; ++i)
    dump(argv[i]);

  std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
              failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

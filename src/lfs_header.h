#ifndef LFS_HEADER_H
#define LFS_HEADER_H

#include <cstdio>
#include <cstring>
#include <stdint.h>

/*
 * LFS capture file format.
 *
 * A .lfs file is a 512-byte packed header followed by a raw stream of
 * interleaved float32 I/Q samples (std::complex<float>, 8 bytes each) at
 * if_rate = sample_rate / dec.
 *
 * This struct is reproduced verbatim from the writer side,
 * gr-juha/include/juha/lfs_header.h (see reference/), and was confirmed
 * byte-for-byte against real captures.
 */

const float    LFS_HEADER_FORMAT_VER = 1.0;
const uint16_t LFS_HEADER_SIZE       = 498;

#pragma pack(push, 1)
struct lfs_header {
  char     format[4];          // 0x000  "LFSG"
  float    format_ver;         // 0x004  1.0
  char     header_id[4];       // 0x008  "fmt "
  uint16_t header_size;        // 0x00C  498
  char     tx_name[64];        // 0x00E
  float    tx_latitude;        // 0x04E
  float    tx_longitude;       // 0x052
  char     rx_name[64];        // 0x056
  float    rx_latitude;        // 0x096
  float    rx_longitude;       // 0x09A
  uint16_t start_year;         // 0x09E
  uint16_t start_daynumber;    // 0x0A0
  uint16_t start_month;        // 0x0A2
  uint16_t start_day;          // 0x0A4
  uint16_t start_hour;         // 0x0A6
  uint16_t start_minute;       // 0x0A8
  uint16_t start_second;       // 0x0AA
  uint32_t start_epoch;        // 0x0AC  unix time, agrees with the fields above
  uint32_t chirpt;             // 0x0B0  chirp start offset within the period, s
  uint32_t cf;                 // 0x0B4  centre frequency, Hz
  uint16_t dur;                // 0x0B8  sounding duration, s
  uint32_t rate;               // 0x0BA  chirp rate, Hz/s
  uint32_t rep;                // 0x0BE  repetition period, s
  int32_t  rmin;               // 0x0C2
  int32_t  rmax;               // 0x0C6
  uint32_t dec;                // 0x0CA  decimation
  uint32_t sample_rate;        // 0x0CE  Hz
  uint16_t whiten;             // 0x0D2
  uint32_t whiten_len;         // 0x0D4
  uint32_t whiten_n;           // 0x0D8
  char     reserved[292];      // 0x0DC .. 0x1FF

  lfs_header()
  {
    /* memcpy, not strncpy: these are fixed 4-char fields with no NUL. */
    memcpy(format, "LFSG", 4);
    format_ver  = LFS_HEADER_FORMAT_VER;
    memcpy(header_id, "fmt ", 4);
    header_size = LFS_HEADER_SIZE;
  }
};
#pragma pack(pop)

/*
 * Read and validate a header from an open file.
 *
 * Returns true only for a well-formed header; on failure the destination is
 * left untouched and the stream position is undefined.
 *
 * Note the version check is exact: captures written by the newer
 * chirpsounder lfs_header.py (format_ver 1.1, header_size 512) are rejected,
 * which is the behaviour of the original binary and is preserved deliberately.
 */
bool lfsheader_read(FILE *file, lfs_header &header);

#endif /* LFS_HEADER_H */

#include "lfs_header.h"

/*
 * Reconstructed from lfsheader_read(_IO_FILE*, lfs_header&) at 0x47fb50.
 *
 * The original reads the whole 512-byte struct in one go, applies four
 * equality checks plus an feof() guard, and only then copies into the
 * caller's header.
 */
bool lfsheader_read(FILE *file, lfs_header &header)
{
  lfs_header tmp;

  if (fread(&tmp, 1, sizeof(lfs_header), file) != sizeof(lfs_header))
    return false;

  if (strncmp(tmp.format, "LFSG", 4) != 0)
    return false;

  if (tmp.format_ver != LFS_HEADER_FORMAT_VER)
    return false;

  if (strncmp(tmp.header_id, "fmt ", 4) != 0)
    return false;

  if (tmp.header_size != LFS_HEADER_SIZE)
    return false;

  if (feof(file))
    return false;

  header = tmp;
  return true;
}

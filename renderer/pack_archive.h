/* Read-only ZIP (.pk3) archives for community texture packs. GPL-2.0-or-later.
 * The central directory is read once; entries are found by name, ignoring
 * case, and their bytes read raw (stored or deflated) so inflating can run on
 * worker threads. ZIP64, encrypted and other compression methods are skipped. */
#ifndef LQ_PACK_ARCHIVE_H
#define LQ_PACK_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#define LQ_ARCHIVE_ENTRY_MAX (256u << 20) /* largest entry read, inflated */

typedef struct lq_archive lq_archive;

/* A raw entry: `data` holds `stored` bytes, which inflate (method 8) or copy
 * (method 0) to `size` bytes with CRC-32 `crc`. */
typedef struct { uint8_t *data; size_t stored, size; uint32_t crc; int method; } lq_archive_blob;

lq_archive *LQ_ArchiveOpen(const char *path);
void LQ_ArchiveClose(lq_archive *archive);
/* Whether any entry's name (lower case) starts with `prefix` and contains `part`. */
int LQ_ArchiveContains(const lq_archive *archive, const char *prefix, const char *part);
/* Reads entry `name` raw; returns 0 when absent or unreadable. */
int LQ_ArchiveReadRaw(lq_archive *archive, const char *name, lq_archive_blob *blob);
/* Turns a raw entry into its bytes in place (any thread). Returns 0, freeing
 * the data, when it does not inflate to its size and CRC. */
int LQ_ArchiveExpand(lq_archive_blob *blob);

#endif

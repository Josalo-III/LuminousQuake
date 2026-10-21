/* Read-only ZIP (.pk3) archives for community texture packs. GPL-2.0-or-later. */
#include "pack_archive.h"
#include "pack_images.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
typedef __int64 archive_offset;
#else
typedef off_t archive_offset;
#endif

typedef struct { char *name; uint64_t local; uint32_t stored, size, crc; uint16_t method; } archive_entry;
struct lq_archive { FILE *file; uint64_t file_size; archive_entry *entries; uint32_t count; char *names; };
/* CRC-32 table, built by LQ_ArchiveOpen on the opening thread before any
 * entry can be expanded on a worker. */
static uint32_t crc_table[256];
static void CrcTable(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t U32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int Seek(FILE *f, uint64_t at) { return at <= (uint64_t)INT64_MAX && fseeko(f, (archive_offset)at, SEEK_SET) == 0; }
static int ReadAt(FILE *f, uint64_t at, void *out, size_t size) { return Seek(f, at) && fread(out, 1, size, f) == size; }
static int CompareEntries(const void *a, const void *b)
{
    return strcmp(((const archive_entry *)a)->name, ((const archive_entry *)b)->name);
}

lq_archive *LQ_ArchiveOpen(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    if (!crc_table[1]) CrcTable();
    lq_archive *a = calloc(1, sizeof(*a));
    uint8_t *directory = NULL, tail[65557];
    if (!a || fseeko(f, 0, SEEK_END) != 0) goto failed;
    a->file = f; a->file_size = (uint64_t)ftello(f);
    /* The end-of-central-directory record: 22 bytes plus a comment of up to 64 KB. */
    size_t span = a->file_size < sizeof(tail) ? (size_t)a->file_size : sizeof(tail);
    if (span < 22 || !ReadAt(f, a->file_size - span, tail, span)) goto failed;
    const uint8_t *end = NULL;
    for (size_t i = span - 22 + 1; i-- > 0;) if (U32(tail + i) == 0x06054b50u) { end = tail + i; break; }
    if (!end) goto failed;
    uint32_t count = U16(end + 10), directory_size = U32(end + 12), directory_at = U32(end + 16);
    if (count == 0xffff || directory_at == 0xffffffffu || (uint64_t)directory_at + directory_size > a->file_size)
        goto failed; /* ZIP64 */
    directory = malloc(directory_size ? directory_size : 1);
    a->entries = calloc(count ? count : 1, sizeof(*a->entries));
    a->names = malloc((size_t)directory_size + 1);
    if (!directory || !a->entries || !a->names || !ReadAt(f, directory_at, directory, directory_size)) goto failed;
    size_t at = 0, names = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 46 > directory_size || U32(directory + at) != 0x02014b50u) goto failed;
        const uint8_t *e = directory + at;
        uint16_t flags = U16(e + 8), method = U16(e + 10), name_length = U16(e + 28);
        size_t record = 46 + (size_t)name_length + U16(e + 30) + U16(e + 32);
        if (at + record > directory_size) goto failed;
        archive_entry entry = {a->names + names, U32(e + 42), U32(e + 20), U32(e + 24), U32(e + 16), method};
        at += record;
        if ((flags & 1) || (method != 0 && method != 8) || entry.stored == 0xffffffffu || entry.size == 0xffffffffu ||
            entry.size > LQ_ARCHIVE_ENTRY_MAX || entry.local >= a->file_size || !name_length) continue;
        for (uint16_t k = 0; k < name_length; ++k)
            a->names[names + k] = (char)tolower(e[46 + k] == '\\' ? '/' : e[46 + k]);
        a->names[names + name_length] = 0;
        names += (size_t)name_length + 1;
        a->entries[a->count++] = entry;
    }
    free(directory);
    qsort(a->entries, a->count, sizeof(*a->entries), CompareEntries);
    return a;
failed:
    free(directory);
    if (a) { free(a->entries); free(a->names); free(a); }
    fclose(f);
    return NULL;
}
void LQ_ArchiveClose(lq_archive *a)
{
    if (!a) return;
    fclose(a->file); free(a->entries); free(a->names); free(a);
}
int LQ_ArchiveContains(const lq_archive *a, const char *prefix, const char *part)
{
    size_t length = strlen(prefix);
    for (uint32_t i = 0; a && i < a->count; ++i)
        if (!strncmp(a->entries[i].name, prefix, length) && strstr(a->entries[i].name + length, part)) return 1;
    return 0;
}
int LQ_ArchiveReadRaw(lq_archive *a, const char *name, lq_archive_blob *blob)
{
    memset(blob, 0, sizeof(*blob));
    char key[256];
    size_t length = strlen(name);
    if (!a || length >= sizeof(key)) return 0;
    for (size_t k = 0; k <= length; ++k) key[k] = (char)tolower((unsigned char)name[k]);
    archive_entry probe = {key, 0, 0, 0, 0, 0};
    const archive_entry *e = bsearch(&probe, a->entries, a->count, sizeof(*a->entries), CompareEntries);
    uint8_t local[30];
    if (!e || !ReadAt(a->file, e->local, local, sizeof(local)) || U32(local) != 0x04034b50u) return 0;
    uint64_t data = e->local + 30 + U16(local + 26) + U16(local + 28);
    if (data + e->stored > a->file_size || (e->method == 0 && e->stored != e->size)) return 0;
    blob->data = malloc(e->stored ? e->stored : 1);
    if (!blob->data || !ReadAt(a->file, data, blob->data, e->stored)) { free(blob->data); blob->data = NULL; return 0; }
    blob->stored = e->stored; blob->size = e->size; blob->crc = e->crc; blob->method = e->method;
    return 1;
}
static uint32_t Crc32(const uint8_t *p, size_t size)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
int LQ_ArchiveExpand(lq_archive_blob *blob)
{
    if (!blob->data) return 0;
    if (blob->method == 8) {
        uint8_t *out = malloc(blob->size ? blob->size : 1);
        int ok = out && LQ_PackInflate(blob->data, blob->stored, out, blob->size);
        free(blob->data);
        blob->data = ok ? out : NULL;
        if (!ok) { free(out); return 0; }
        blob->stored = blob->size; blob->method = 0;
    }
    if (Crc32(blob->data, blob->size) != blob->crc) { free(blob->data); blob->data = NULL; return 0; }
    return 1;
}

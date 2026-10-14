/* Optional English lookup from the installation, never bundled game data.
 * GPL-2.0-or-later. ZIP reader supports a bounded, single non-ZIP64 entry. */
#include "quakedef.h"
#include "localization.h"
#include <ctype.h>
#include <zlib.h>
typedef struct { char *key, *value; } entry_t;
static entry_t entries[4096];
static size_t entry_count;
static char *table;
static unsigned U16(const byte *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned U32(const byte *p) { return U16(p) | U16(p+2) << 16; }
static char *ReadKPF(const char *path)
{
    const char *wanted = "localization/loc_english.txt";
    FILE *f = fopen(path, "rb");
    byte *tail = NULL, *packed = NULL;
    char *result = NULL;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) goto done;
    long length = ftell(f);
    if (length < 22) goto done;
    long size = length < 65557 ? length : 65557;
    tail = malloc(size);
    if (!tail || fseek(f, length-size, SEEK_SET) || fread(tail, 1, size, f) != (size_t)size) goto done;
    long end;
    for (end = size-22; end >= 0; --end)
        if (U32(tail+end) == 0x06054b50 && end+22+U16(tail+end+20) == size) break;
    if (end < 0 || U16(tail+end+4) || U16(tail+end+6)) goto done;
    unsigned count = U16(tail+end+10), offset = U32(tail+end+16);
    if (offset >= (unsigned long)length || fseek(f, offset, SEEK_SET)) goto done;
    for (unsigned i = 0; i < count; ++i) {
        byte header[46]; char name[256];
        if (fread(header, 1, 46, f) != 46 || U32(header) != 0x02014b50) goto done;
        unsigned n = U16(header+28), extra = U16(header+30)+U16(header+32);
        if (n >= sizeof(name)) { if (fseek(f, n+extra, SEEK_CUR)) goto done; continue; }
        if (fread(name, 1, n, f) != n) goto done;
        name[n] = 0;
        if (strcmp(name, wanted)) { if (fseek(f, extra, SEEK_CUR)) goto done; continue; }
        unsigned compressed = U32(header+20), unpacked = U32(header+24), method = U16(header+10);
        unsigned crc = U32(header+16), local = U32(header+42);
        if (U16(header+8)&1 || !unpacked || unpacked > 4*1024*1024 || compressed > 4*1024*1024 ||
            local >= (unsigned long)length || (method != 0 && method != 8)) goto done;
        byte h[30];
        if (fseek(f, local, SEEK_SET) || fread(h, 1, 30, f) != 30 || U32(h) != 0x04034b50 ||
            fseek(f, U16(h+26)+U16(h+28), SEEK_CUR)) goto done;
        packed = malloc(compressed ? compressed : 1); result = malloc(unpacked+1);
        if (!packed || !result || fread(packed, 1, compressed, f) != compressed) goto invalid;
        if (method == 0) {
            if (compressed != unpacked) goto invalid;
            memcpy(result, packed, unpacked);
        } else {
            z_stream stream = {0};
            stream.next_in = packed; stream.avail_in = compressed;
            stream.next_out = (byte *)result; stream.avail_out = unpacked;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) goto invalid;
            int status = inflate(&stream, Z_FINISH);
            int valid = status == Z_STREAM_END && stream.total_out == unpacked;
            inflateEnd(&stream);
            if (!valid) goto invalid;
        }
        if (crc32(0, (byte *)result, unpacked) != crc) goto invalid;
        result[unpacked] = 0;
        goto done;
invalid:
        free(result); result = NULL; goto done;
    }
done:
    free(tail); free(packed); fclose(f); return result;
}
void LQ_LocalizationParse(char *text)
{
    entry_count = 0;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *p = line;
        while (isspace((unsigned char)*p)) ++p;
        char *key = p;
        while (isalnum((unsigned char)*p) || *p == '_') ++p;
        if (p == key) { line = next; continue; }
        char *key_end = p;
        while (isspace((unsigned char)*p)) ++p;
        if (*p++ != '=') { line = next; continue; }
        while (isspace((unsigned char)*p)) ++p;
        if (*p++ != '"') { line = next; continue; }
        *key_end = 0;
        char *value = p, *out = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                ++p;
                *out++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                ++p;
            } else *out++ = *p++;
        }
        if (*p == '"' && entry_count < sizeof(entries)/sizeof(entries[0])) {
            *out = 0; entries[entry_count++] = (entry_t){key, value};
        }
        line = next;
    }
}
void LQ_LocalizationInit(const char *basedir)
{
    free(table); table = NULL; entry_count = 0;
    /* Normal search paths allow loose or PAK-based localization for mods. */
    int handle, size = COM_OpenFile("localization/loc_english.txt", &handle);
    if (size >= 0) {
        if (size <= 4*1024*1024) {
            table = malloc((size_t)size+1);
            if (table) { Sys_FileRead(handle, table, size); table[size] = 0; }
        }
        COM_CloseFile(handle);
    }
    if (!table) {
        char path[MAX_OSPATH];
        if (snprintf(path, sizeof(path), "%s/QuakeEX.kpf", basedir) < (int)sizeof(path))
            table = ReadKPF(path);
    }
    if (table) {
        LQ_LocalizationParse(table);
        Sys_Printf("English localization: %zu entries\n", entry_count);
    }
}
const char *LQ_Localize(const char *text)
{
    if (!text || text[0] != '$') return text;
    for (size_t i = 0; i < entry_count; ++i)
        if (!strcmp(text+1, entries[i].key)) return entries[i].value;
    return text; /* Preserve unknown keys and all original game text. */
}
int LQ_LocalizeArgs(int argc, const char *const *argv, char *output, unsigned size)
{
    unsigned used = 0;
    if (!size) return 0;
    output[0] = 0;
    for (int i = 0; i < argc; ++i) {
        const char *literal = argv[i], *text = LQ_Localize(literal);
        int localized = text != literal;
        int consumed = 0;
        while (*text) {
            const char *value = NULL;
            unsigned length = 1;
            /* Only table strings are templates. Literal braces in original
             * game messages/player names never become format instructions. */
            if (localized && text[0] == '{' && text[1] >= '0' && text[1] <= '7' && text[2] == '}') {
                int argument = text[1]-'0'+1;
                if (i+argument < argc) {
                    value = LQ_Localize(argv[i+argument]); length = (unsigned)strlen(value);
                    if (argument > consumed) consumed = argument;
                    text += 3;
                }
            }
            if (!value) value = text++;
            if (length >= size-used) { output[used] = 0; return 0; }
            memcpy(output+used,value,length); used += length;
        }
        i += consumed;
    }
    output[used] = 0; return 1;
}

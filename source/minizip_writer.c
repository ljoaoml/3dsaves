#include "minizip_writer.h"
#include <stdlib.h>
#include <string.h>

// --- CRC32 (IEEE 802.3 / zip polynomial 0xEDB88320) ------------------------

static uint32_t s_crcTable[256];
static bool s_crcTableInit = false;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crcTable[i] = c;
    }
    s_crcTableInit = true;
}

static uint32_t crc32_buf(const uint8_t *data, uint32_t len) {
    if (!s_crcTableInit) crc32_init_table();
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c = s_crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// --- ZIP writer --------------------------------------------------------

typedef struct {
    char *name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
} ZipEntry;

struct ZipWriter {
    FILE *f;
    uint32_t offset;
    ZipEntry *entries;
    uint32_t entryCount;
    uint32_t entryCap;
};

static void put_u16(FILE *f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
    fwrite(b, 1, 2, f);
}

static void put_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
    fwrite(b, 1, 4, f);
}

ZipWriter *zipw_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    ZipWriter *zw = calloc(1, sizeof(ZipWriter));
    zw->f = f;
    zw->entryCap = 64;
    zw->entries = malloc(sizeof(ZipEntry) * zw->entryCap);
    return zw;
}

bool zipw_add_file(ZipWriter *zw, const char *name, const uint8_t *data, uint32_t size) {
    if (!zw || !zw->f) return false;

    if (zw->entryCount == zw->entryCap) {
        zw->entryCap *= 2;
        zw->entries = realloc(zw->entries, sizeof(ZipEntry) * zw->entryCap);
    }

    uint32_t crc = crc32_buf(data, size);
    uint16_t nameLen = (uint16_t)strlen(name);
    uint32_t localOffset = zw->offset;

    put_u32(zw->f, 0x04034b50);   // local file header signature
    put_u16(zw->f, 20);           // version needed to extract
    put_u16(zw->f, 0);            // flags
    put_u16(zw->f, 0);            // compression = store
    put_u16(zw->f, 0);            // mod time
    put_u16(zw->f, 0x21);         // mod date (1980-01-01, valid minimum DOS date)
    put_u32(zw->f, crc);
    put_u32(zw->f, size);         // compressed size
    put_u32(zw->f, size);         // uncompressed size
    put_u16(zw->f, nameLen);
    put_u16(zw->f, 0);            // extra field length
    fwrite(name, 1, nameLen, zw->f);
    if (size > 0) fwrite(data, 1, size, zw->f);

    zw->offset = localOffset + 30 + nameLen + size;

    ZipEntry *e = &zw->entries[zw->entryCount++];
    e->name = strdup(name);
    e->crc = crc;
    e->size = size;
    e->offset = localOffset;

    return true;
}

bool zipw_close(ZipWriter *zw) {
    if (!zw || !zw->f) return false;

    uint32_t cdStart = zw->offset;

    for (uint32_t i = 0; i < zw->entryCount; i++) {
        ZipEntry *e = &zw->entries[i];
        uint16_t nameLen = (uint16_t)strlen(e->name);

        put_u32(zw->f, 0x02014b50); // central directory file header signature
        put_u16(zw->f, 20);         // version made by
        put_u16(zw->f, 20);         // version needed to extract
        put_u16(zw->f, 0);          // flags
        put_u16(zw->f, 0);          // compression = store
        put_u16(zw->f, 0);          // mod time
        put_u16(zw->f, 0x21);       // mod date
        put_u32(zw->f, e->crc);
        put_u32(zw->f, e->size);    // compressed size
        put_u32(zw->f, e->size);    // uncompressed size
        put_u16(zw->f, nameLen);
        put_u16(zw->f, 0);          // extra length
        put_u16(zw->f, 0);          // comment length
        put_u16(zw->f, 0);          // disk number start
        put_u16(zw->f, 0);          // internal file attributes
        put_u32(zw->f, 0);          // external file attributes
        put_u32(zw->f, e->offset);  // relative offset of local header
        fwrite(e->name, 1, nameLen, zw->f);

        zw->offset += 46 + nameLen;
    }

    uint32_t cdSize = zw->offset - cdStart;

    put_u32(zw->f, 0x06054b50);          // end of central directory signature
    put_u16(zw->f, 0);                   // disk number
    put_u16(zw->f, 0);                   // disk with central directory
    put_u16(zw->f, (uint16_t)zw->entryCount); // entries on this disk
    put_u16(zw->f, (uint16_t)zw->entryCount); // total entries
    put_u32(zw->f, cdSize);
    put_u32(zw->f, cdStart);
    put_u16(zw->f, 0);                   // comment length

    fclose(zw->f);

    for (uint32_t i = 0; i < zw->entryCount; i++) free(zw->entries[i].name);
    free(zw->entries);
    free(zw);
    return true;
}

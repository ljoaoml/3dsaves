#include "minizip_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define EOCD_SIG      0x06054b50u
#define CDH_SIG       0x02014b50u
#define LFH_SIG       0x04034b50u
#define EOCD_SIZE     22
#define CDH_FIXED_LEN 46
#define LFH_FIXED_LEN 30

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

// Creates every directory component of `path` (an sdmc:/... path),
// tolerating components that already exist -- same shape as main.c's
// fixed mkdir("sdmc:/3ds", ...); mkdir("sdmc:/3ds/Konnect3DS", ...) calls,
// just walking an arbitrary-depth path instead of two fixed ones.
static void mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

bool zipr_extract_all(const char *zipPath, const char *destDir) {
    FILE *f = fopen(zipPath, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long fileSize = ftell(f);
    if (fileSize < EOCD_SIZE) { fclose(f); return false; }

    // minizip_writer.c always writes a zero-length archive comment, so
    // the EOCD record is always exactly the last 22 bytes -- no need for
    // the general "search backward up to 64KB" dance a reader for
    // arbitrary zips (with a comment) would need.
    if (fseek(f, fileSize - EOCD_SIZE, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t eocd[EOCD_SIZE];
    if (fread(eocd, 1, EOCD_SIZE, f) != EOCD_SIZE) { fclose(f); return false; }
    if (get_u32(eocd) != EOCD_SIG) { fclose(f); return false; }

    uint16_t totalEntries = get_u16(eocd + 10);
    uint32_t cdOffset = get_u32(eocd + 16);

    if (fseek(f, (long)cdOffset, SEEK_SET) != 0) { fclose(f); return false; }

    mkdir_p(destDir);

    for (uint16_t i = 0; i < totalEntries; i++) {
        uint8_t cdh[CDH_FIXED_LEN];
        if (fread(cdh, 1, sizeof(cdh), f) != sizeof(cdh)) { fclose(f); return false; }
        if (get_u32(cdh) != CDH_SIG) { fclose(f); return false; }

        uint16_t method = get_u16(cdh + 10);
        uint32_t compSize = get_u32(cdh + 20);
        uint16_t nameLen = get_u16(cdh + 28);
        uint16_t extraLen = get_u16(cdh + 30);
        uint16_t commentLen = get_u16(cdh + 32);
        uint32_t localOffset = get_u32(cdh + 42);

        char name[512];
        if (nameLen == 0 || nameLen >= sizeof(name)) { fclose(f); return false; }
        if (fread(name, 1, nameLen, f) != nameLen) { fclose(f); return false; }
        name[nameLen] = '\0';

        // Skip this entry's extra field + comment to land on the next
        // central directory header.
        if (fseek(f, extraLen + commentLen, SEEK_CUR) != 0) { fclose(f); return false; }
        long cdReturnPos = ftell(f);

        if (method == 0) { // STORE; anything else is silently skipped
            if (fseek(f, (long)localOffset, SEEK_SET) != 0) { fclose(f); return false; }
            uint8_t lfh[LFH_FIXED_LEN];
            if (fread(lfh, 1, sizeof(lfh), f) != sizeof(lfh)) { fclose(f); return false; }
            if (get_u32(lfh) == LFH_SIG) {
                uint16_t lfNameLen = get_u16(lfh + 26);
                uint16_t lfExtraLen = get_u16(lfh + 28);
                if (fseek(f, lfNameLen + lfExtraLen, SEEK_CUR) == 0) {
                    char destPath[768];
                    snprintf(destPath, sizeof(destPath), "%s/%s", destDir, name);

                    // A zip entry name may itself contain '/'-separated
                    // subdirectories (a save's own folder structure) --
                    // make sure they exist before writing the file.
                    char *lastSlash = strrchr(destPath, '/');
                    if (lastSlash) {
                        *lastSlash = '\0';
                        mkdir_p(destPath);
                        *lastSlash = '/';
                    }

                    FILE *out = fopen(destPath, "wb");
                    if (out) {
                        uint8_t buf[4096];
                        uint32_t remaining = compSize;
                        while (remaining > 0) {
                            uint32_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
                            if (fread(buf, 1, chunk, f) != chunk) break;
                            fwrite(buf, 1, chunk, out);
                            remaining -= chunk;
                        }
                        fclose(out);
                    }
                }
            }
        }

        if (fseek(f, cdReturnPos, SEEK_SET) != 0) { fclose(f); return false; }
    }

    fclose(f);
    return true;
}

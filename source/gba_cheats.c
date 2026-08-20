#include "gba_cheats.h"
#include <stdio.h>
#include <string.h>

void gba_cheats_path_for_rom(const char *romPath, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    size_t len = strlen(romPath);
    const char *dot = strrchr(romPath, '.');
    // Only treat it as an extension if the dot is after the last path
    // separator (otherwise a dotted directory name would confuse this).
    const char *slash = strrchr(romPath, '/');
    if (dot && (!slash || dot > slash)) {
        len = (size_t)(dot - romPath);
    }
    if (len >= outSize) len = outSize - 1;
    memcpy(out, romPath, len);
    snprintf(out + len, outSize - len, ".cheats");
}

// mGBA's own cheat-set header line, from GBACheatParseFile()/mCheatSaveFile()
// in mGBA's source: a set starts with "# <name>" and every following line up
// to the next "#" line (or EOF) is a raw code line belonging to that set.
static bool is_name_line(const char *line, const char **outName) {
    if (line[0] != '#') return false;
    const char *p = line + 1;
    while (*p == ' ') p++;
    if (outName) *outName = p;
    return true;
}

void gba_cheats_list(const char *cheatsPath, GbaCheatEntry *out, int maxEntries, int *outCount) {
    int count = 0;
    if (outCount) *outCount = 0;
    FILE *f = fopen(cheatsPath, "r");
    if (!f) return;

    char line[256];
    while (count < maxEntries && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        const char *name;
        if (is_name_line(line, &name)) {
            snprintf(out[count].name, sizeof(out[count].name), "%s", name);
            count++;
        }
    }

    fclose(f);
    if (outCount) *outCount = count;
}

bool gba_cheats_add(const char *cheatsPath, const char *name, const char *codeLine) {
    FILE *f = fopen(cheatsPath, "ab");
    if (!f) return false;

    // A single "# name\ncode\n" entry is tiny (well under the SD card
    // write-size limit that forced chunked writes elsewhere in this app),
    // so one fprintf() call is safe here.
    int written = fprintf(f, "# %s\n%s\n", name, codeLine);
    fclose(f);
    return written > 0;
}

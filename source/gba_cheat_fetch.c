#include "gba_cheat_fetch.h"
#include "gba_cheats.h"
#include "http.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define GBA_CHEATS_INDEX_PATH "romfs:/gba_cheats_index.txt"
#define GBA_CHEATS_RAW_BASE \
    "https://raw.githubusercontent.com/libretro/libretro-database/master/" \
    "cht/Nintendo%20-%20Game%20Boy%20Advance/"

bool gba_rom_read_title(const char *romPath, char *out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';

    FILE *f = fopen(romPath, "rb");
    if (!f) return false;

    char raw[12];
    bool ok = fseek(f, 0xA0, SEEK_SET) == 0 && fread(raw, 1, sizeof(raw), f) == sizeof(raw);
    fclose(f);
    if (!ok) return false;

    int len = 0;
    for (int i = 0; i < 12; i++) {
        if (raw[i] == '\0') break;
        len++;
    }
    while (len > 0 && raw[len - 1] == ' ') len--; // trim trailing header padding

    size_t n = (size_t)len < outSize - 1 ? (size_t)len : outSize - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
    return true;
}

// Normalizes `src` into `out`: uppercased, '-' treated as a space, runs of
// whitespace collapsed to one, no leading/trailing space. Lets a GBA header
// title ("POKEMON EMER", no punctuation) be compared directly against a
// No-Intro-style filename title ("Pokemon - Emerald Version").
static void normalize_title(const char *src, char *out, size_t outSize) {
    size_t o = 0;
    bool pendingSpace = false;
    for (size_t i = 0; src[i] && o + 1 < outSize; i++) {
        char c = src[i];
        if (c == '-') c = ' ';
        if (isspace((unsigned char)c)) {
            if (o > 0) pendingSpace = true;
            continue;
        }
        if (pendingSpace) {
            out[o++] = ' ';
            pendingSpace = false;
            if (o + 1 >= outSize) break;
        }
        out[o++] = (char)toupper((unsigned char)c);
    }
    out[o] = '\0';
}

int gba_cheat_search(const char *romTitle, char candidatesOut[][GBA_CHEAT_FILENAME_MAX],
                      int maxCandidates) {
    char normRomTitle[64];
    normalize_title(romTitle, normRomTitle, sizeof(normRomTitle));
    size_t romTitleLen = strlen(normRomTitle);
    // Too short to match reliably -- e.g. a homebrew/blank header -- would
    // otherwise prefix-match almost every entry in the index.
    if (romTitleLen < 3) return 0;

    FILE *f = fopen(GBA_CHEATS_INDEX_PATH, "r");
    if (!f) return 0;

    int count = 0;
    char line[GBA_CHEAT_FILENAME_MAX];
    while (count < maxCandidates && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        const char *paren = strchr(line, '(');
        size_t titleLen = paren ? (size_t)(paren - line) : len;

        char titlePart[GBA_CHEAT_FILENAME_MAX];
        size_t n = titleLen < sizeof(titlePart) - 1 ? titleLen : sizeof(titlePart) - 1;
        memcpy(titlePart, line, n);
        titlePart[n] = '\0';

        char normFileTitle[128];
        normalize_title(titlePart, normFileTitle, sizeof(normFileTitle));

        if (strncmp(normFileTitle, normRomTitle, romTitleLen) == 0) {
            snprintf(candidatesOut[count], GBA_CHEAT_FILENAME_MAX, "%s", line);
            count++;
        }
    }
    fclose(f);
    return count;
}

// mCheatSaveFile()/GBACheatAddLine()'s own CodeBreaker autodetection (see
// gba_cheats.h) reads a clean "AAAAAAAA VVVV" line unambiguously as
// CodeBreaker. libretro-database's plain-hex cheatN_code values already are
// that shape for the vast majority of GBA entries (cross-checked several
// samples directly, including files with no "(Code Breaker)" tag at all
// that turned out to use the exact same shape) -- so rather than trust each
// file's own name/label, this converts by verifying the actual byte shape
// token by token: a clean run of 8-hex-digit/4-hex-digit pairs joined by
// '+'. A file that turned out to hold genuine encrypted GameShark data
// (checked one directly: its tokens are all 8 hex digits, not this 8/4
// alternating shape) fails this check and is skipped, rather than risk
// writing a cheat whose address/value split was guessed wrong.
static bool convert_code_to_lines(const char *code, char *linesOut, size_t linesOutSize) {
    size_t o = 0;
    const char *p = code;
    bool any = false;

    while (*p) {
        char addr[9];
        int ai = 0;
        while (isxdigit((unsigned char)*p) && ai < 8) addr[ai++] = *p++;
        if (ai != 8 || isxdigit((unsigned char)*p)) return false;
        addr[ai] = '\0';

        if (*p != '+') return false;
        p++;

        char val[5];
        int vi = 0;
        while (isxdigit((unsigned char)*p) && vi < 4) val[vi++] = *p++;
        if (vi != 4 || isxdigit((unsigned char)*p)) return false;
        val[vi] = '\0';

        size_t room = o < linesOutSize ? linesOutSize - o : 0;
        int n = snprintf(linesOut + o, room, "%s %s\n", addr, val);
        if (n < 0 || (size_t)n >= room) return false; // ran out of room
        o += (size_t)n;
        any = true;

        if (*p == '+') { p++; continue; }
        if (*p == '\0') break;
        return false; // trailing garbage that isn't another pair
    }
    return any;
}

// Matches lines like "cheat<N>_desc"/"cheat<N>_code" -- N can be any digit
// run, so this checks the shape (prefix "cheat" + digits + suffix) rather
// than a fixed key. Deliberately does NOT match the file's own leading
// "cheats = N" count line (no digits directly after "cheat").
static bool line_matches_key(const char *line, const char *suffix) {
    if (strncmp(line, "cheat", 5) != 0) return false;
    const char *p = line + 5;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    return strncmp(p, suffix, strlen(suffix)) == 0;
}

static bool parse_quoted_value(const char *line, char *out, size_t outSize) {
    const char *eq = strchr(line, '=');
    if (!eq) return false;
    const char *p = eq + 1;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < outSize) out[o++] = *p++;
    out[o] = '\0';
    return true;
}

bool gba_cheat_fetch_and_write(const char *filename, const char *cheatsPath,
                                int *outWritten, int *outSkipped,
                                char *errorOut, size_t errorOutSize) {
    if (outWritten) *outWritten = 0;
    if (outSkipped) *outSkipped = 0;

    char encodedName[GBA_CHEAT_FILENAME_MAX * 3];
    http_url_encode(filename, encodedName, sizeof(encodedName));

    char url[700];
    snprintf(url, sizeof(url), GBA_CHEATS_RAW_BASE "%s", encodedName);

    HttpResponse resp;
    Result rc = http_request(HTTPC_METHOD_GET, url, NULL, 0, NULL, 0, &resp);
    if (R_FAILED(rc)) {
        if (errorOut) snprintf(errorOut, errorOutSize, "couldn't reach the cheat database");
        return false;
    }
    if (resp.status_code != 200 || !resp.body.data) {
        if (errorOut) {
            snprintf(errorOut, errorOutSize, "cheat file not found (HTTP %lu)",
                     (unsigned long)resp.status_code);
        }
        http_response_free(&resp);
        return false;
    }

    // Cheats already present -- skip re-adding them so repeated searches
    // (or searching a ROM whose .cheats file already has manually-copied
    // entries) don't pile up duplicates.
    GbaCheatEntry existing[64];
    int existingCount = 0;
    gba_cheats_list(cheatsPath, existing, 64, &existingCount);

    const char *base = (const char *)resp.body.data;
    u32 size = resp.body.size;
    u32 i = 0;

    char pendingDesc[128] = {0};
    bool haveDesc = false;
    int written = 0, skipped = 0;
    bool writeFailed = false;

    while (i < size) {
        u32 start = i;
        while (i < size && base[i] != '\n') i++;
        u32 lineLen = i - start;
        if (i < size) i++; // skip the newline itself

        char line[512];
        u32 n = lineLen < sizeof(line) - 1 ? lineLen : sizeof(line) - 1;
        memcpy(line, base + start, n);
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';

        const char *trimmed = line;
        while (*trimmed == ' ') trimmed++;

        if (line_matches_key(trimmed, "_desc")) {
            haveDesc = parse_quoted_value(trimmed, pendingDesc, sizeof(pendingDesc));
            continue;
        }

        if (line_matches_key(trimmed, "_code") && haveDesc) {
            char code[512];
            if (parse_quoted_value(trimmed, code, sizeof(code))) {
                bool alreadyThere = false;
                for (int e = 0; e < existingCount; e++) {
                    if (strcmp(existing[e].name, pendingDesc) == 0) { alreadyThere = true; break; }
                }

                if (!alreadyThere) {
                    char cheatLines[512];
                    if (convert_code_to_lines(code, cheatLines, sizeof(cheatLines))) {
                        if (!gba_cheats_add_raw(cheatsPath, pendingDesc, cheatLines)) {
                            writeFailed = true;
                            break;
                        }
                        written++;
                    } else {
                        skipped++;
                    }
                }
            }
            haveDesc = false;
        }
    }

    http_response_free(&resp);

    if (writeFailed) {
        if (errorOut) snprintf(errorOut, errorOutSize, "failed to save cheats (check free space)");
        return false;
    }

    if (outWritten) *outWritten = written;
    if (outSkipped) *outSkipped = skipped;
    return true;
}

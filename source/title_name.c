// SMDH read technique adapted from selloa/3DS-Random-Game-Launcher
// (source/title_smdh.c, MIT license) -- trimmed to just the short
// application name, since that's all this project's title list needs.
#include "title_name.h"
#include <3ds/services/cfgu.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_ICON_SIZE 0x36C0

typedef struct {
    u16 short_desc[0x40];
    u16 long_desc[0x80];
    u16 publisher[0x40];
} SMDH_ApplicationTitle;

// Fields nobody here reads individually, but their exact sizes are what
// make bigIconData land at the right offset -- struct layout cross-checked
// against Checkpoint's own smdh.hpp (FlagBrew/Checkpoint), whose settings
// field sizes sum to the same 0x36C0 total this file already used for
// TITLE_ICON_SIZE before this struct grew far enough to prove it.
typedef struct {
    u8 game_ratings[0x10];
    u32 region_lock;
    u8 matchmaker_id[0xC];
    u32 flags;
    u16 eula_version;
    u16 reserved;
    u32 default_frame;
    u32 cec_id;
} SMDH_Settings;

typedef struct {
    u32 magic;
    u16 version;
    u16 reserved;
    SMDH_ApplicationTitle titles[16];
    SMDH_Settings settings;
    u8 reserved2[0x8];
    u8 small_icon_data[0x480];  // 24x24 RGB565, unused here
    u16 big_icon_data[0x900];   // 48x48 RGB565, tiled (see title_get_info())
} SMDH_Header;

static u8 get_smdh_language_index(void) {
    u8 lang = 1; // English, used if CFGU is unavailable
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemLanguage(&lang);
        cfguExit();
    }
    if (lang > 11) lang = 1;
    return lang;
}

// Reads the title's ExeFS:/icon file (the SMDH) into `iconData`
// (TITLE_ICON_SIZE bytes). This is the documented way to get another
// installed title's icon/name without launching it.
static Result read_icon_file(u64 titleId, FS_MediaType media, u8 *iconData) {
    u32 low = (u32)titleId;
    u32 high = (u32)(titleId >> 32);
    u32 archPath[] = { low, high, (u32)media, 0 };
    u32 filePath[] = { 0, 0, 2, 0x6E6F6369, 0 }; // ExeFS section path for "icon"
    FS_Path archFsPath = { PATH_BINARY, sizeof(archPath), archPath };
    FS_Path fileFsPath = { PATH_BINARY, sizeof(filePath), filePath };

    Handle handle = 0;
    Result res = FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT,
                                          archFsPath, fileFsPath, FS_OPEN_READ, 0);
    FS_Archive archive = 0;
    if (R_FAILED(res)) {
        res = FSUSER_OpenArchive(&archive, ARCHIVE_SAVEDATA_AND_CONTENT, archFsPath);
        if (R_FAILED(res)) return res;

        res = FSUSER_OpenFile(&handle, archive, fileFsPath, FS_OPEN_READ, 0);
        if (R_FAILED(res)) {
            FSUSER_CloseArchive(archive);
            return res;
        }
    }

    u32 bytesRead = 0;
    res = FSFILE_Read(handle, &bytesRead, 0, iconData, TITLE_ICON_SIZE);
    FSFILE_Close(handle);
    if (archive != 0) FSUSER_CloseArchive(archive);

    if (R_FAILED(res)) return res;
    if (bytesRead < sizeof(SMDH_Header)) return -1;
    return 0;
}

// Transliterates the Latin-1 Supplement's accented letters (U+00C0-U+00FF,
// the ones actually common in the mostly-English/Spanish/Portuguese/French
// game titles this reads) to their plain-ASCII base letter -- 'e' for
// both 'e'/'é'/'è'/'ê'/'ë', etc. Returns 0 for anything outside that block
// (including the punctuation/multiplication-sign gaps within it), which
// utf16_field_to_ascii() below then falls back to '?' for, same as before
// this existed. Without this, a title like "Pokémon" became "Pok?mon" --
// not just a cosmetic wart, since the icon grid's search tile matches
// against this same converted string, so searching "pokemon" could never
// find it.
static char latin1_accent_to_ascii(u16 c) {
    // Index i is codepoint (base + i); the embedded '\0's line up with the
    // two non-letter codepoints in these ranges (multiplication/division
    // signs) and correctly fall through to utf16_field_to_ascii()'s '?'.
    static const char upper[] = "AAAAAAACEEEEIIIIDNOOOOO\0OUUUUYP"; // U+00C0-DE: ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞ
    static const char lower[] = "saaaaaaaceeeeiiiidnooooo\0ouuuuypy"; // U+00DF-FF: ßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ
    if (c >= 0xC0 && c <= 0xDE) return upper[c - 0xC0];
    if (c >= 0xDF && c <= 0xFF) return lower[c - 0xDF];
    return 0;
}

// Converts a NUL-terminated UTF-16 field to plain ASCII: printable ASCII
// characters pass through, common Latin accented letters are stripped to
// their base letter (see latin1_accent_to_ascii()), everything else
// becomes '?'. Not a real Unicode->console-font mapping, just enough to
// avoid printing garbage multi-byte sequences through a text renderer
// with no Unicode font, while keeping accented titles searchable/readable
// as their plain-letter spelling instead of full of '?'.
static void utf16_field_to_ascii(const u16 *in, size_t inMaxChars, char *out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; i < inMaxChars && in[i] != 0 && o + 1 < outSize; i++) {
        u16 c = in[i];
        if (c >= 0x20 && c <= 0x7E) {
            out[o++] = (char)c;
            continue;
        }
        char accented = latin1_accent_to_ascii(c);
        out[o++] = accented ? accented : '?';
    }
    out[o] = '\0';
}

bool title_get_info(u64 titleId, FS_MediaType media,
                     char *nameOut, size_t nameOutSize, u16 *iconOut) {
    if (nameOut && nameOutSize > 0) nameOut[0] = '\0';
    if (iconOut) memset(iconOut, 0, TITLE_BIG_ICON_PIXELS * sizeof(u16));

    u8 *iconData = malloc(TITLE_ICON_SIZE);
    if (!iconData) return false;

    Result res = read_icon_file(titleId, media, iconData);
    if (R_FAILED(res)) {
        free(iconData);
        return false;
    }

    SMDH_Header *smdh = (SMDH_Header *)iconData;
    if (memcmp(&smdh->magic, "SMDH", 4) != 0) {
        free(iconData);
        return false;
    }

    if (nameOut && nameOutSize > 0) {
        u8 lang = get_smdh_language_index();
        const u16 *field = smdh->titles[lang].short_desc;
        if (field[0] == 0) field = smdh->titles[1].short_desc; // fall back to English
        if (field[0] != 0) utf16_field_to_ascii(field, 0x40, nameOut, nameOutSize);
    }

    if (iconOut) memcpy(iconOut, smdh->big_icon_data, TITLE_BIG_ICON_PIXELS * sizeof(u16));

    free(iconData);
    return true;
}

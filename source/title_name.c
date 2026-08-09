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

typedef struct {
    u32 magic;
    u16 version;
    u16 reserved;
    SMDH_ApplicationTitle titles[16];
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

// Converts a NUL-terminated UTF-16 field to plain ASCII: printable
// characters pass through, everything else becomes '?'. Not a real
// Unicode->console-font mapping, just enough to avoid printing garbage
// multi-byte sequences through a text renderer with no Unicode font.
static void utf16_field_to_ascii(const u16 *in, size_t inMaxChars, char *out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; i < inMaxChars && in[i] != 0 && o + 1 < outSize; i++) {
        u16 c = in[i];
        out[o++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    out[o] = '\0';
}

bool title_get_name(u64 titleId, FS_MediaType media, char *out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';

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

    u8 lang = get_smdh_language_index();
    const u16 *field = smdh->titles[lang].short_desc;
    if (field[0] == 0) field = smdh->titles[1].short_desc; // fall back to English

    if (field[0] == 0) {
        free(iconData);
        return false;
    }

    utf16_field_to_ascii(field, 0x40, out, outSize);
    free(iconData);
    return out[0] != '\0';
}

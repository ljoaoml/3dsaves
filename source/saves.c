#include "saves.h"
#include "minizip_writer.h"
#include "title_name.h"

#include <3ds.h>
#include <3ds/util/utf.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Regular retail/eShop applications live under the 0x00040000 high title-ID
// category; system apps/modules/applets use other categories. Filtering on
// this is the same approach FBI/Checkpoint-style tools use to list "things
// that plausibly have a game save" instead of every installed system title.
#define TITLE_CATEGORY_APPLICATION 0x00040000ULL

static bool is_application_title(u64 titleId) {
    return ((titleId >> 32) & 0xFFFFFFFFULL) == TITLE_CATEGORY_APPLICATION;
}

// A handful of built-in system titles share the 0x00040000 application
// category with real games but obviously aren't games -- ported verbatim
// from Checkpoint's TitleQuirks::isSystemExcluded() (3ds/source/
// titlequirks.cpp), the exact IDs it excludes across every region variant
// of the Instruction Manual and Internet Browser, plus one title it
// documents only as "Garbage". Region-specific low IDs don't follow any
// derivable pattern, so this is a lookup table, not a formula, same as
// upstream.
static bool is_system_excluded(u64 titleId) {
    switch ((u32)titleId) {
        case 0x00008602: case 0x00009202: case 0x00009B02: // Instruction Manual
        case 0x0000A402: case 0x0000AC02: case 0x0000B402:
        case 0x00008802: case 0x00009402: case 0x00009D02: // Internet Browser
        case 0x0000A602: case 0x0000AE02: case 0x0000B602:
        case 0x20008802: case 0x20009402: case 0x20009D02:
        case 0x2000AE02:
        case 0x00021A00: // "Garbage" (Checkpoint's own label for this one)
            return true;
        default:
            break;
    }
    if ((titleId >> 32) == 0x0004000EULL) return true;  // title updates
    if ((titleId >> 32) == 0x0004800FULL) return true;  // DSi non-executable data archives
    return false;
}

static Result open_title_save_archive(const InstalledTitle *title, FS_Archive *archive);

// Cheap accessibility probe: open the archive, then immediately close it
// without reading anything -- matches Checkpoint's SaveDataSource::
// accessible() for the CtrSave/Extdata cases (a raw GBA VC save or a TWL
// title needs more than this, but neither applies to SD/cartridge CTR
// titles, the only kind this project lists).
static bool savedata_accessible(const InstalledTitle *title) {
    FS_Archive archive;
    Result rc = open_title_save_archive(title, &archive);
    if (R_FAILED(rc)) return false;
    FSUSER_CloseArchive(archive);
    return true;
}

// Most titles' extdata archive ID is just their own unique ID (the same
// value Checkpoint's own paths.cpp uses for the "0x%05X " Checkpoint
// folder prefix -- see checkpoint_saves.c), but a handful of specific
// titles' extdata lives under a different ID that doesn't follow that
// pattern at all. Ported verbatim from Checkpoint's TitleQuirks::
// extdataIdFor() (3ds/source/titlequirks.cpp); matches on the *whole* low
// 32 bits of the title ID, not just the unique-ID portion, same as
// upstream.
static u32 extdata_id_for(u64 titleId) {
    u32 low = (u32)titleId;
    switch (low) {
        case 0x00055E00: return 0x055D; // Pokémon Y
        case 0x0011C400: return 0x11C5; // Pokémon Omega Ruby
        case 0x00175E00: return 0x1648; // Pokémon Moon
        case 0x00179600: case 0x00179800: return 0x1794; // Fire Emblem Conquest SE NA
        case 0x00179700: case 0x0017A800: return 0x1795; // Fire Emblem Conquest SE EU
        case 0x0012DD00: case 0x0012DE00: return 0x12DC; // Fire Emblem If JP
        case 0x001B5100: return 0x1B50; // Pokémon Ultramoon
        default: return low >> 8;
    }
}

static Result open_extdata_archive(u32 extdataId, FS_Archive *archive) {
    u32 path[3] = { (u32)MEDIATYPE_SD, extdataId, 0 };
    FS_Path binPath = { PATH_BINARY, sizeof(path), path };
    return FSUSER_OpenArchive(archive, ARCHIVE_EXTDATA, binPath);
}

static bool extdata_accessible(u64 titleId) {
    FS_Archive archive;
    Result rc = open_extdata_archive(extdata_id_for(titleId), &archive);
    if (R_FAILED(rc)) return false;
    FSUSER_CloseArchive(archive);
    return true;
}

// --- GBA Virtual Console injects ---------------------------------------
//
// A GBA VC inject's save never lives in the normal SAVEDATA archive at all
// (savedata_accessible() above fails for it) -- it's a separate AGBSAVE
// container only reachable through the PxiFS0 service, which a regular app
// has no session to. Checkpoint reaches it by "stealing" one via
// svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, ...), an undocumented
// syscall added by Luma3DS's kernel patches (not a stock ARM11 syscall, and
// not part of stock libctru -- ported from Checkpoint's own csvc.hpp,
// itself adapted from Luma3DS). On any other CFW (or none) this call just
// fails and GBA VC titles silently go back to not showing up, same as
// before this existed.
typedef enum {
    SERVICEOP_STEAL_CLIENT_SESSION = 0,
    SERVICEOP_GET_NAME = 1,
} ServiceOp;

extern Result svcControlService(ServiceOp op, ...);

static Handle s_fsPxiHandle = 0;
static bool s_fsPxiTried = false;
static bool s_fsPxiOk = false;

static bool ensure_fspxi(void) {
    if (s_fsPxiTried) return s_fsPxiOk;
    s_fsPxiTried = true;
    Result rc = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &s_fsPxiHandle, "PxiFS0");
    s_fsPxiOk = R_SUCCEEDED(rc);
    return s_fsPxiOk;
}

static Result open_raw_gba_archive(const InstalledTitle *title, FSPXI_Archive *archive) {
    if (!ensure_fspxi()) return -1;
    u32 path[4] = {
        (u32)(title->titleId & 0xFFFFFFFFULL),
        (u32)(title->titleId >> 32),
        (u32)title->mediaType,
        1,
    };
    FS_Path binPath = { PATH_BINARY, sizeof(path), path };
    return FSPXI_OpenArchive(s_fsPxiHandle, archive, ARCHIVE_SAVEDATA_AND_CONTENT, binPath);
}

// GBA VC saves are always exposed by FSPXI under this fixed binary path
// (ported from Checkpoint's fsstream.cpp, which names it pxi_path).
static const u32 s_rawGbaFilePath[5] = { 1, 1, 3, 0, 0 };

// Matches Checkpoint's SaveDataSource::accessible() for the RawGbaSave
// case exactly: the archive/file opening cleanly is the whole check. It
// says nothing about whether a save has actually been written yet (see
// gba_locate_current_slot() below) -- same as Checkpoint, that's a
// backup-time failure ("launch the game and save once"), not a listing
// exclusion, so a title that's never been saved still shows up.
static bool rawgba_save_accessible(const InstalledTitle *title) {
    FSPXI_Archive archive;
    Result rc = open_raw_gba_archive(title, &archive);
    if (R_FAILED(rc)) return false;

    FSPXI_File file;
    FS_Path filePath = { PATH_BINARY, sizeof(s_rawGbaFilePath), s_rawGbaFilePath };
    rc = FSPXI_OpenFile(s_fsPxiHandle, &file, archive, filePath, FS_OPEN_READ, 0);
    bool ok = R_SUCCEEDED(rc);
    if (ok) FSPXI_CloseFile(s_fsPxiHandle, file);
    FSPXI_CloseArchive(s_fsPxiHandle, archive);
    return ok;
}

// The AGBSAVE container format (see
// https://www.3dbrew.org/wiki/3DS_Virtual_Console#NAND_Savegame and
// Checkpoint's gbasave.cpp, which this is ported from). Only the read side
// is ported -- locating the current slot and pulling its bare save bytes
// out -- since this app never writes into a title's live save archive
// (see download_and_prepare_restore()'s doc comment in main.c); the CMAC
// re-signing a restore needs is Checkpoint's job, done when the user
// finishes the restore there. As long as what we hand it is the exact bare
// save (one of s_gbaValidSaveSizes below, no header), Checkpoint's own
// restore() recognizes it as such and re-signs it itself.
typedef struct {
    u8 magic[4]; // ".SAV"
    u8 reserved0[0xC];
    u8 cmac[0x10];
    u8 reserved1[0x10];
    u32 unknown0;
    u32 timesSaved;
    u64 titleId;
    u8 sdCid[0x10];
    u32 saveStart; // always 0x200
    u32 saveSize;
    u8 reserved2[0x8];
    u32 unknown1;
    u32 unknown2;
    u8 reserved3[0x198];
} __attribute__((packed)) AgbSaveHeader;
_Static_assert(sizeof(AgbSaveHeader) == 0x200, "AgbSaveHeader must be 0x200 bytes");

static const u32 s_gbaValidSaveSizes[] = { 512, 8 * 1024, 32 * 1024, 64 * 1024, 128 * 1024 };

static bool gba_valid_save_size(u32 size) {
    for (size_t i = 0; i < sizeof(s_gbaValidSaveSizes) / sizeof(s_gbaValidSaveSizes[0]); i++) {
        if (size == s_gbaValidSaveSizes[i]) return true;
    }
    return false;
}

static bool gba_valid_header(const AgbSaveHeader *hdr) {
    return memcmp(hdr->magic, ".SAV", 4) == 0 && hdr->saveStart == (u32)sizeof(AgbSaveHeader) &&
           gba_valid_save_size(hdr->saveSize);
}

static bool gba_read_header_at(FSPXI_File file, u32 offset, AgbSaveHeader *out) {
    u32 bytesRead = 0;
    Result rc = FSPXI_ReadFile(s_fsPxiHandle, file, &bytesRead, offset, out, (u32)sizeof(*out));
    return R_SUCCEEDED(rc) && bytesRead == (u32)sizeof(*out) && gba_valid_header(out);
}

// Mirrors Checkpoint's GbaSave::locateCurrentSlot(): the container has two
// save slots; whichever has the higher timesSaved counter is current (the
// top slot wins ties). If the top slot has never been written, the bottom
// slot's own offset depends on its (as yet unknown) save size, so every
// valid size is tried there instead.
static bool gba_locate_current_slot(FSPXI_File file, AgbSaveHeader *hdrOut, u32 *slotOffsetOut) {
    AgbSaveHeader top;
    if (gba_read_header_at(file, 0, &top)) {
        AgbSaveHeader bottom;
        u32 bottomOffset = (u32)sizeof(AgbSaveHeader) + top.saveSize;
        if (gba_read_header_at(file, bottomOffset, &bottom) && bottom.timesSaved > top.timesSaved) {
            *hdrOut = bottom;
            *slotOffsetOut = bottomOffset;
        } else {
            *hdrOut = top;
            *slotOffsetOut = 0;
        }
        return true;
    }
    for (size_t i = 0; i < sizeof(s_gbaValidSaveSizes) / sizeof(s_gbaValidSaveSizes[0]); i++) {
        u32 bottomOffset = (u32)sizeof(AgbSaveHeader) + s_gbaValidSaveSizes[i];
        if (gba_read_header_at(file, bottomOffset, hdrOut)) {
            *slotOffsetOut = bottomOffset;
            return true;
        }
    }
    return false;
}

// saves_backup_title()'s path for title->isRawGbaSave -- a single bare
// save file instead of walk_and_zip()'s whole-directory-tree dump, since
// there's nothing else to preserve. "00000001.sav" is the exact filename
// Checkpoint's own GbaSave::backup() writes and GbaSave::restore() looks
// for first inside a backup-instance folder (rawBackupFile() in io.cpp),
// so a Checkpoint-driven restore (see download_and_prepare_restore() in
// main.c) finds it without any renaming.
static Result backup_raw_gba_save(const InstalledTitle *title, const char *zipPath) {
    FSPXI_Archive archive;
    Result rc = open_raw_gba_archive(title, &archive);
    if (R_FAILED(rc)) return rc;

    FSPXI_File file;
    FS_Path filePath = { PATH_BINARY, sizeof(s_rawGbaFilePath), s_rawGbaFilePath };
    rc = FSPXI_OpenFile(s_fsPxiHandle, &file, archive, filePath, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) { FSPXI_CloseArchive(s_fsPxiHandle, archive); return rc; }

    AgbSaveHeader hdr;
    u32 slotOffset = 0;
    if (!gba_locate_current_slot(file, &hdr, &slotOffset)) {
        FSPXI_CloseFile(s_fsPxiHandle, file);
        FSPXI_CloseArchive(s_fsPxiHandle, archive);
        return -1; // container exists but was never saved to -- launch the game once
    }

    u8 *buf = malloc(hdr.saveSize);
    if (!buf) {
        FSPXI_CloseFile(s_fsPxiHandle, file);
        FSPXI_CloseArchive(s_fsPxiHandle, archive);
        return -1;
    }

    u32 bytesRead = 0;
    rc = FSPXI_ReadFile(s_fsPxiHandle, file, &bytesRead, slotOffset + (u32)sizeof(AgbSaveHeader), buf, hdr.saveSize);
    FSPXI_CloseFile(s_fsPxiHandle, file);
    FSPXI_CloseArchive(s_fsPxiHandle, archive);

    if (R_FAILED(rc) || bytesRead != hdr.saveSize) {
        free(buf);
        return R_FAILED(rc) ? rc : -1;
    }

    ZipWriter *zw = zipw_open(zipPath);
    if (!zw) { free(buf); return -1; }
    zipw_add_file(zw, "00000001.sav", buf, hdr.saveSize);
    zipw_close(zw);
    free(buf);
    return 0;
}

// Identifies a physical DS/DSi cartridge sitting in the game card slot --
// deliberately just the ROM header (game title/code), nothing from the
// save chip at all: no SPI reads, no writes, none of the risk that comes
// with actually touching the cart's save memory (unlike GBA VC's
// isRawGbaSave path above, this project makes no attempt to read a
// physical DS cart's save itself -- Checkpoint's own SPI driver for that
// is ~600 lines of chip-specific protocol quirks, and even probing a
// chip's *type* can involve a test write for some EEPROM sizes). Returns
// false if no card is inserted, it's a normal 3DS card (FSUSER_GetCardType()
// == CARD_CTR, nothing special to report), or the header can't be read.
// On true, `gameCode` (e.g. "IPKE") and `description` (the ROM's own
// 12-character title) are both filled in, NUL-terminated -- matching
// exactly what Checkpoint's own probeCard() (3ds/source/titleprobe.cpp)
// uses to name this cart's Checkpoint folder, so the two agree on where
// its backups would live.
bool saves_detect_ds_card(char *gameCode, size_t gameCodeSize, char *description, size_t descSize) {
    if (gameCodeSize > 0) gameCode[0] = '\0';
    if (descSize > 0) description[0] = '\0';

    FS_CardType cardType;
    Result rc = FSUSER_GetCardType(&cardType);
    if (R_FAILED(rc) || cardType == CARD_CTR) return false;

    u8 header[0x3B4];
    rc = FSUSER_GetLegacyRomHeader(MEDIATYPE_GAME_CARD, 0, header);
    if (R_FAILED(rc)) return false;

    char title[13] = {0};
    memcpy(title, header, 12);
    char code[5] = {0};
    memcpy(code, header + 12, 4);

    // Trim trailing NULs/space padding the ROM header's fixed-width title
    // field leaves behind, same as Checkpoint's own probeCard().
    for (int i = 11; i >= 0 && (title[i] == '\0' || title[i] == ' '); i--) title[i] = '\0';

    snprintf(gameCode, gameCodeSize, "%s", code);
    snprintf(description, descSize, "%s", title[0] ? title : code);
    return true;
}

// `applyFilters` true is the everyday saves_list_titles() behavior
// (system-title exclusion + an accessible-save requirement); false is
// saves_list_all_titles()'s "show literally everything" fallback -- same
// loop either way, just skipping the two exclusion checks so a title an
// edge case in those checks still hides gets a place to be found and
// tried anyway. isRawGbaSave/extdataAccessible are still probed and
// filled in correctly in both cases, so the normal backup functions work
// identically on a title found this way.
static Result list_titles_for_media(FS_MediaType mediatype, bool applyFilters, InstalledTitle **arr,
                                     u32 *count, u32 *capacity,
                                     TitleScanProgressFn onProgress, void *userdata) {
    u32 total = 0;
    Result rc = AM_GetTitleCount(mediatype, &total);
    if (R_FAILED(rc) || total == 0) return 0;

    u64 *ids = malloc(sizeof(u64) * total);
    if (!ids) return -1;

    u32 read = 0;
    rc = AM_GetTitleList(&read, mediatype, total, ids);
    if (R_FAILED(rc)) { free(ids); return rc; }

    for (u32 i = 0; i < read; i++) {
        if (onProgress) onProgress(i + 1, read, userdata);
        if (!is_application_title(ids[i])) continue;
        if (applyFilters && is_system_excluded(ids[i])) continue;

        InstalledTitle candidate;
        candidate.titleId = ids[i];
        candidate.mediaType = mediatype;
        // savedata_accessible()/rawgba_save_accessible() only need
        // titleId/mediaType, filled in above; probe before growing the
        // array or touching the SMDH so a title that fails both probes
        // costs nothing more than opening and immediately closing an
        // archive. A GBA VC inject fails the normal SAVEDATA probe (its
        // save isn't there at all -- see the "GBA Virtual Console
        // injects" section above) but still has a real save reachable
        // the other way, so it isn't excluded, just flagged.
        bool isRawGba = false;
        bool accessible = savedata_accessible(&candidate);
        if (!accessible) {
            accessible = rawgba_save_accessible(&candidate);
            isRawGba = accessible;
        }
        if (applyFilters && !accessible) continue;

        if (*count == *capacity) {
            u32 newCapacity = (*capacity == 0) ? 16 : (*capacity * 2);
            InstalledTitle *grown = realloc(*arr, sizeof(InstalledTitle) * newCapacity);
            if (!grown) break; // out of memory; keep what's been found so far
            *arr = grown;
            *capacity = newCapacity;
        }

        InstalledTitle *t = &(*arr)[*count];
        t->titleId = ids[i];
        t->mediaType = mediatype;
        memset(t->productCode, 0, sizeof(t->productCode));
        AM_GetTitleProductCode(mediatype, ids[i], t->productCode);
        // Best-effort: leaves t->name empty and t->icon all-zero if the
        // title has no readable SMDH, callers fall back to showing the
        // product code / a plain placeholder instead.
        title_get_info(ids[i], mediatype, t->name, sizeof(t->name), t->icon);
        t->extdataAccessible = extdata_accessible(ids[i]);
        t->isRawGbaSave = isRawGba;
        (*count)++;
    }

    free(ids);
    return 0;
}

// Plain strcmp, not a case-insensitive compare -- devkitARM's newlib
// doesn't guarantee strcasecmp is present in every config (see http.c's
// do_single_request() comment on the same tradeoff), and real game
// titles overwhelmingly start with a capital letter anyway, so a plain
// ASCII sort already reads as alphabetical order in practice.
static int compare_titles_by_name(const void *a, const void *b) {
    const InstalledTitle *ta = (const InstalledTitle *)a;
    const InstalledTitle *tb = (const InstalledTitle *)b;
    const char *nameA = ta->name[0] ? ta->name : ta->productCode;
    const char *nameB = tb->name[0] ? tb->name : tb->productCode;
    return strcmp(nameA, nameB);
}

Result saves_list_titles(InstalledTitle **out, u32 *count,
                          TitleScanProgressFn onProgress, void *userdata) {
    *out = NULL;
    *count = 0;
    u32 capacity = 0;

    Result rc = amInit();
    if (R_FAILED(rc)) return rc;

    Result rc1 = list_titles_for_media(MEDIATYPE_SD, true, out, count, &capacity, onProgress, userdata);
    Result rc2 = list_titles_for_media(MEDIATYPE_GAME_CARD, true, out, count, &capacity, onProgress, userdata);

    amExit();

    if (R_FAILED(rc1)) return rc1;
    if (R_FAILED(rc2)) return rc2;
    if (*count > 0) qsort(*out, *count, sizeof(InstalledTitle), compare_titles_by_name);
    return 0;
}

Result saves_list_all_titles(InstalledTitle **out, u32 *count,
                              TitleScanProgressFn onProgress, void *userdata) {
    *out = NULL;
    *count = 0;
    u32 capacity = 0;

    Result rc = amInit();
    if (R_FAILED(rc)) return rc;

    Result rc1 = list_titles_for_media(MEDIATYPE_SD, false, out, count, &capacity, onProgress, userdata);
    Result rc2 = list_titles_for_media(MEDIATYPE_GAME_CARD, false, out, count, &capacity, onProgress, userdata);

    amExit();

    if (R_FAILED(rc1)) return rc1;
    if (R_FAILED(rc2)) return rc2;
    if (*count > 0) qsort(*out, *count, sizeof(InstalledTitle), compare_titles_by_name);
    return 0;
}

static Result open_title_save_archive(const InstalledTitle *title, FS_Archive *archive) {
    u32 path[3];
    path[0] = (u32)title->mediaType;
    path[1] = (u32)(title->titleId & 0xFFFFFFFFULL);
    path[2] = (u32)(title->titleId >> 32);

    FS_Path binPath = { PATH_BINARY, sizeof(path), path };
    return FSUSER_OpenArchive(archive, ARCHIVE_USER_SAVEDATA, binPath);
}

// Recursively walks `fsDir` inside `archive`, adding every file found to
// `zw` with a zip entry name of `zipPrefix` + the file's path.
static Result walk_and_zip(FS_Archive archive, const char *fsDir, const char *zipPrefix,
                            ZipWriter *zw, int depth) {
    if (depth > 32) return 0; // pathological loop guard

    Handle dirHandle;
    FS_Path dirPath = fsMakePath(PATH_ASCII, fsDir);
    Result rc = FSUSER_OpenDirectory(&dirHandle, archive, dirPath);
    if (R_FAILED(rc)) return rc;

    FS_DirectoryEntry entry;
    for (;;) {
        u32 entriesRead = 0;
        rc = FSDIR_Read(dirHandle, &entriesRead, 1, &entry);
        if (R_FAILED(rc) || entriesRead == 0) break;

        char nameUtf8[0x106 * 4];
        ssize_t nameLen = utf16_to_utf8((uint8_t *)nameUtf8, entry.name,
                                         sizeof(nameUtf8) - 1);
        if (nameLen < 0) continue;
        nameUtf8[nameLen] = '\0';
        if (nameLen == 0) continue;

        char childFsPath[512];
        bool dirIsRoot = (fsDir[0] == '/' && fsDir[1] == '\0');
        if (dirIsRoot) {
            snprintf(childFsPath, sizeof(childFsPath), "/%s", nameUtf8);
        } else {
            snprintf(childFsPath, sizeof(childFsPath), "%s/%s", fsDir, nameUtf8);
        }

        char childZipName[512];
        snprintf(childZipName, sizeof(childZipName), "%s%s", zipPrefix, nameUtf8);

        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
            char childZipPrefix[512];
            snprintf(childZipPrefix, sizeof(childZipPrefix), "%s/", childZipName);
            walk_and_zip(archive, childFsPath, childZipPrefix, zw, depth + 1);
            continue;
        }

        Handle fileHandle;
        FS_Path filePath = fsMakePath(PATH_ASCII, childFsPath);
        rc = FSUSER_OpenFile(&fileHandle, archive, filePath, FS_OPEN_READ, 0);
        if (R_FAILED(rc)) continue;

        u64 size64 = 0;
        FSFILE_GetSize(fileHandle, &size64);
        u32 size = (u32)size64;

        u8 *buf = size > 0 ? malloc(size) : NULL;
        if (size > 0 && !buf) {
            FSFILE_Close(fileHandle);
            continue; // out of memory; skip this file rather than aborting the whole backup
        }

        u32 bytesRead = 0;
        if (size > 0) FSFILE_Read(fileHandle, &bytesRead, 0, buf, size);
        FSFILE_Close(fileHandle);

        zipw_add_file(zw, childZipName, buf, size);
        if (buf) free(buf);
    }

    FSDIR_Close(dirHandle);
    return 0;
}

Result saves_backup_title(const InstalledTitle *title, const char *zipPath) {
    if (title->isRawGbaSave) return backup_raw_gba_save(title, zipPath);

    FS_Archive archive;
    Result rc = open_title_save_archive(title, &archive);
    if (R_FAILED(rc)) return rc;

    ZipWriter *zw = zipw_open(zipPath);
    if (!zw) {
        FSUSER_CloseArchive(archive);
        return -1;
    }

    walk_and_zip(archive, "/", "", zw, 0);

    zipw_close(zw);
    FSUSER_CloseArchive(archive);
    return 0;
}

Result saves_backup_title_extdata(const InstalledTitle *title, const char *zipPath) {
    FS_Archive archive;
    Result rc = open_extdata_archive(extdata_id_for(title->titleId), &archive);
    if (R_FAILED(rc)) return rc;

    ZipWriter *zw = zipw_open(zipPath);
    if (!zw) {
        FSUSER_CloseArchive(archive);
        return -1;
    }

    walk_and_zip(archive, "/", "", zw, 0);

    zipw_close(zw);
    FSUSER_CloseArchive(archive);
    return 0;
}

// Recursively walks `diskDir` (a plain sdmc:/... path) adding every file
// found to `zw` with a zip entry name of `zipPrefix` + the file's path.
// Mirrors walk_and_zip() above but over stdio instead of an FS_Archive,
// since a Checkpoint-style backup already sits on the SD card as loose
// files, not inside a title's own save archive.
static void walk_and_zip_folder(const char *diskDir, const char *zipPrefix,
                                 ZipWriter *zw, int depth) {
    if (depth > 32) return; // pathological loop guard

    DIR *dir = opendir(diskDir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char childDiskPath[512];
        snprintf(childDiskPath, sizeof(childDiskPath), "%s/%s", diskDir, entry->d_name);
        char childZipName[512];
        snprintf(childZipName, sizeof(childZipName), "%s%s", zipPrefix, entry->d_name);

        struct stat st;
        if (stat(childDiskPath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            char childZipPrefix[512];
            snprintf(childZipPrefix, sizeof(childZipPrefix), "%s/", childZipName);
            walk_and_zip_folder(childDiskPath, childZipPrefix, zw, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;

        FILE *f = fopen(childDiskPath, "rb");
        if (!f) continue;

        u32 size = st.st_size > 0 ? (u32)st.st_size : 0;
        u8 *buf = size > 0 ? malloc(size) : NULL;
        if (size > 0 && !buf) {
            fclose(f);
            continue; // out of memory; skip this file rather than aborting the whole backup
        }

        u32 bytesRead = size > 0 ? (u32)fread(buf, 1, size, f) : 0;
        fclose(f);
        if (bytesRead != size) {
            if (buf) free(buf);
            continue; // short read; skip rather than zip a truncated/garbage file
        }

        zipw_add_file(zw, childZipName, buf, size);
        if (buf) free(buf);
    }

    closedir(dir);
}

Result saves_backup_folder(const char *sourceDir, const char *zipPath) {
    DIR *probe = opendir(sourceDir);
    if (!probe) return -1;
    closedir(probe);

    ZipWriter *zw = zipw_open(zipPath);
    if (!zw) return -1;

    walk_and_zip_folder(sourceDir, "", zw, 0);

    zipw_close(zw);
    return 0;
}

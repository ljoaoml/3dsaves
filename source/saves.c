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

static Result list_titles_for_media(FS_MediaType mediatype, InstalledTitle **arr,
                                     u32 *count, u32 *capacity) {
    u32 total = 0;
    Result rc = AM_GetTitleCount(mediatype, &total);
    if (R_FAILED(rc) || total == 0) return 0;

    u64 *ids = malloc(sizeof(u64) * total);
    if (!ids) return -1;

    u32 read = 0;
    rc = AM_GetTitleList(&read, mediatype, total, ids);
    if (R_FAILED(rc)) { free(ids); return rc; }

    for (u32 i = 0; i < read; i++) {
        if (!is_application_title(ids[i])) continue;
        if (is_system_excluded(ids[i])) continue;

        InstalledTitle candidate;
        candidate.titleId = ids[i];
        candidate.mediaType = mediatype;
        // savedata_accessible() only needs titleId/mediaType, filled in
        // above; probe before growing the array or touching the SMDH so a
        // title that fails the probe costs nothing more than opening and
        // immediately closing its archive.
        if (!savedata_accessible(&candidate)) continue;

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
        (*count)++;
    }

    free(ids);
    return 0;
}

Result saves_list_titles(InstalledTitle **out, u32 *count) {
    *out = NULL;
    *count = 0;
    u32 capacity = 0;

    Result rc = amInit();
    if (R_FAILED(rc)) return rc;

    Result rc1 = list_titles_for_media(MEDIATYPE_SD, out, count, &capacity);
    Result rc2 = list_titles_for_media(MEDIATYPE_GAME_CARD, out, count, &capacity);

    amExit();

    if (R_FAILED(rc1)) return rc1;
    if (R_FAILED(rc2)) return rc2;
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

#include "saves.h"
#include "minizip_writer.h"
#include "title_name.h"

#include <3ds.h>
#include <3ds/util/utf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Regular retail/eShop applications live under the 0x00040000 high title-ID
// category; system apps/modules/applets use other categories. Filtering on
// this is the same approach FBI/Checkpoint-style tools use to list "things
// that plausibly have a game save" instead of every installed system title.
#define TITLE_CATEGORY_APPLICATION 0x00040000ULL

static bool is_application_title(u64 titleId) {
    return ((titleId >> 32) & 0xFFFFFFFFULL) == TITLE_CATEGORY_APPLICATION;
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

        if (*count == *capacity) {
            *capacity = (*capacity == 0) ? 16 : (*capacity * 2);
            *arr = realloc(*arr, sizeof(InstalledTitle) * (*capacity));
        }

        InstalledTitle *t = &(*arr)[*count];
        t->titleId = ids[i];
        t->mediaType = mediatype;
        memset(t->productCode, 0, sizeof(t->productCode));
        AM_GetTitleProductCode(mediatype, ids[i], t->productCode);
        // Best-effort: leaves t->name empty if the title has no readable
        // SMDH, callers fall back to showing the product code instead.
        title_get_name(ids[i], mediatype, t->name, sizeof(t->name));
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

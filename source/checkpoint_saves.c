#include "checkpoint_saves.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Returns true and fills `out` (an sdmc:/... path, no trailing slash) if
// exactly one subfolder of CHECKPOINT_SAVES_DIR starts with titleId's
// "0x%05X " unique-ID prefix. False if CHECKPOINT_SAVES_DIR can't be
// opened, or nothing matches (title has no Checkpoint folder yet).
static bool find_title_folder(u64 titleId, char *out, size_t outSize) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "0x%05X ", (unsigned int)((u32)titleId >> 8));
    size_t prefixLen = strlen(prefix);

    DIR *d = opendir(CHECKPOINT_SAVES_DIR);
    if (!d) return false;

    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefixLen) != 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", CHECKPOINT_SAVES_DIR, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        snprintf(out, outSize, "%s", full);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

static int compare_names_descending(const void *a, const void *b) {
    return strcmp(((const CheckpointBackup *)b)->name, ((const CheckpointBackup *)a)->name);
}

static int compare_folders_by_name(const void *a, const void *b) {
    return strcmp(((const CheckpointTitleFolder *)a)->name, ((const CheckpointTitleFolder *)b)->name);
}

// Shared by checkpoint_list_backups() (which resolves `titleFolder` from a
// titleId first) and checkpoint_list_backups_in_folder() (which already
// has it, from checkpoint_list_title_folders()): lists every
// backup-instance subfolder directly inside `titleFolder`, newest-first
// (plain descending name sort -- Checkpoint's own default backup folder
// names are sortable timestamps, and title.cpp's own loadBackupList()
// sorts saves the same way).
static bool list_instances_in_folder(const char *titleFolder, CheckpointBackup *out, int maxBackups, int *outCount) {
    DIR *d = opendir(titleFolder);
    if (!d) return true; // folder vanished/isn't readable; zero backups, not a failure

    int count = 0;
    struct dirent *entry;
    while (count < maxBackups && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", titleFolder, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        CheckpointBackup *b = &out[count];
        snprintf(b->name, sizeof(b->name), "%s", entry->d_name);
        snprintf(b->fullPath, sizeof(b->fullPath), "%s", full);
        count++;
    }
    closedir(d);

    qsort(out, count, sizeof(CheckpointBackup), compare_names_descending);

    if (outCount) *outCount = count;
    return true;
}

bool checkpoint_list_backups(u64 titleId, CheckpointBackup *out, int maxBackups, int *outCount) {
    if (outCount) *outCount = 0;

    char titleFolder[512];
    if (!find_title_folder(titleId, titleFolder, sizeof(titleFolder))) {
        // Checkpoint saves dir itself might not exist either -- tell those
        // two cases apart so the caller can report "Checkpoint not
        // installed" vs. just "no backups for this title" if it wants to.
        DIR *d = opendir(CHECKPOINT_SAVES_DIR);
        if (!d) return false;
        closedir(d);
        return true; // dir exists, this title just has no folder in it
    }

    return list_instances_in_folder(titleFolder, out, maxBackups, outCount);
}

bool checkpoint_list_backups_in_folder(const char *titleFolder, CheckpointBackup *out, int maxBackups, int *outCount) {
    if (outCount) *outCount = 0;
    return list_instances_in_folder(titleFolder, out, maxBackups, outCount);
}

bool checkpoint_list_title_folders(CheckpointTitleFolder *out, int maxFolders, int *outCount) {
    if (outCount) *outCount = 0;

    DIR *d = opendir(CHECKPOINT_SAVES_DIR);
    if (!d) return false; // Checkpoint's never been run on this SD card

    int count = 0;
    struct dirent *entry;
    while (count < maxFolders && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", CHECKPOINT_SAVES_DIR, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        CheckpointTitleFolder *f = &out[count];
        snprintf(f->name, sizeof(f->name), "%s", entry->d_name);
        snprintf(f->fullPath, sizeof(f->fullPath), "%s", full);

        // "0x%05X " prefix -- see CHECKPOINT_SAVES_DIR's own comment.
        // uniqueId stays 0 (no real title uses that as its own unique id)
        // if the folder doesn't actually start with one, e.g. something
        // dropped in by hand rather than by Checkpoint itself.
        f->uniqueId = 0;
        unsigned int parsed = 0;
        if (sscanf(entry->d_name, "0x%5X ", &parsed) == 1) f->uniqueId = parsed;

        count++;
    }
    closedir(d);

    qsort(out, count, sizeof(CheckpointTitleFolder), compare_folders_by_name);

    if (outCount) *outCount = count;
    return true;
}

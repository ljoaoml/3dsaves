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

    DIR *d = opendir(titleFolder);
    if (!d) return true; // matched a folder that vanished/isn't readable; zero backups

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

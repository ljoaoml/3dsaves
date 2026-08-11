#include "sd_browse.h"
#include "ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_ROOT "sdmc:/"

typedef struct {
    char **names;   // subfolder names (not full paths), in readdir() order
    int count;
    bool hasParent; // true unless currentDir is SD_ROOT
} BrowseCtx;

static bool is_sd_root(const char *path) {
    return strcmp(path, SD_ROOT) == 0;
}

// Truncates `path` (an sdmc:/... path with no trailing slash, except for
// SD_ROOT itself) to its parent directory in place. Caller must not call
// this when is_sd_root(path) is already true.
static void go_to_parent(char *path) {
    char *lastSlash = strrchr(path, '/');
    if (!lastSlash) return; // shouldn't happen, every valid path here starts with SD_ROOT
    if (lastSlash == path + (sizeof(SD_ROOT) - 2)) {
        // Only the SD_ROOT's own slash remains -- parent is the root itself.
        path[sizeof(SD_ROOT) - 1] = '\0';
    } else {
        *lastSlash = '\0';
    }
}

// Frees ctx->names and resets count/hasParent; call before listing a new
// directory so a picker session doesn't leak one array per navigation step.
static void free_listing(BrowseCtx *ctx) {
    for (int i = 0; i < ctx->count; i++) free(ctx->names[i]);
    free(ctx->names);
    ctx->names = NULL;
    ctx->count = 0;
}

static int compare_names(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool list_subfolders(const char *dir, BrowseCtx *ctx) {
    free_listing(ctx);

    DIR *d = opendir(dir);
    if (!d) return false;

    int capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full[600];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (ctx->count == capacity) {
            capacity = capacity == 0 ? 16 : capacity * 2;
            char **grown = realloc(ctx->names, sizeof(char *) * capacity);
            if (!grown) break; // out of memory; show what's been found so far
            ctx->names = grown;
        }
        ctx->names[ctx->count] = strdup(entry->d_name);
        if (ctx->names[ctx->count]) ctx->count++;
    }
    closedir(d);

    // readdir() order is raw filesystem order (arbitrary on FAT/exFAT),
    // not alphabetical -- sort so navigating a folder with many
    // subfolders is actually predictable.
    if (ctx->count > 0) qsort(ctx->names, ctx->count, sizeof(char *), compare_names);
    return true;
}

// Menu layout: [0] "Select this folder", optionally [1] "Parent folder",
// then one entry per subfolder.
static int first_subfolder_index(const BrowseCtx *ctx) {
    return ctx->hasParent ? 2 : 1;
}

static const char *browse_label(int index, void *userdata) {
    BrowseCtx *ctx = (BrowseCtx *)userdata;
    if (index == 0) return "[Select this folder]";
    if (ctx->hasParent && index == 1) return "[.. Parent folder]";
    return ctx->names[index - first_subfolder_index(ctx)];
}

bool sd_browse_pick_folder(const char *startDir, char *outPath, size_t outSize) {
    char currentDir[512];
    snprintf(currentDir, sizeof(currentDir), "%s", startDir);

    BrowseCtx ctx = {0};
    bool result = false;

    for (;;) {
        ctx.hasParent = !is_sd_root(currentDir);
        if (!list_subfolders(currentDir, &ctx)) {
            // Can't even open this directory (shouldn't normally happen for
            // a path we just navigated into) -- back out rather than get stuck.
            break;
        }

        int total = first_subfolder_index(&ctx) + ctx.count;
        int choice = ui_run_menu(currentDir, total, browse_label, &ctx);

        if (choice < 0) break; // B: cancel the whole picker

        if (choice == 0) {
            snprintf(outPath, outSize, "%s", currentDir);
            result = true;
            break;
        }
        if (ctx.hasParent && choice == 1) {
            go_to_parent(currentDir);
            continue;
        }

        const char *sub = ctx.names[choice - first_subfolder_index(&ctx)];
        char next[512];
        snprintf(next, sizeof(next), "%s/%s", currentDir, sub);
        snprintf(currentDir, sizeof(currentDir), "%s", next);
    }

    free_listing(&ctx);
    return result;
}

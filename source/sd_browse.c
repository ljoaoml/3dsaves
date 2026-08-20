#include "sd_browse.h"
#include "ui.h"

#include <ctype.h>
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

// Joins `dir` + '/' + `name` without producing a double slash when `dir`
// already ends in one -- true only for SD_ROOT itself ("sdmc:/"), every
// other currentDir value here has no trailing slash. An unnoticed doubled
// slash (e.g. "sdmc://3ds") makes stat()/opendir() fail on real hardware,
// which silently hid every entry at the SD root as if it were empty.
static void join_path(char *out, size_t outSize, const char *dir, const char *name) {
    size_t dirLen = strlen(dir);
    if (dirLen > 0 && dir[dirLen - 1] == '/') {
        snprintf(out, outSize, "%s%s", dir, name);
    } else {
        snprintf(out, outSize, "%s/%s", dir, name);
    }
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
        join_path(full, sizeof(full), dir, entry->d_name);
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
        join_path(next, sizeof(next), currentDir, sub);
        snprintf(currentDir, sizeof(currentDir), "%s", next);
    }

    free_listing(&ctx);
    return result;
}

// --- Single-file picker (folders as pure navigation, matching files as the
// terminal selection) -------------------------------------------------

static bool has_extension_ci(const char *name, const char *extension) {
    size_t nameLen = strlen(name);
    size_t extLen = strlen(extension);
    if (extLen > nameLen) return false;
    const char *tail = name + (nameLen - extLen);
    for (size_t i = 0; i < extLen; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)extension[i])) return false;
    }
    return true;
}

typedef struct {
    char **dirNames;
    int dirCount;
    char **fileNames; // names matching the requested extension only
    int fileCount;
    bool hasParent;
} FileBrowseCtx;

static void free_file_listing(FileBrowseCtx *ctx) {
    for (int i = 0; i < ctx->dirCount; i++) free(ctx->dirNames[i]);
    free(ctx->dirNames);
    ctx->dirNames = NULL;
    ctx->dirCount = 0;
    for (int i = 0; i < ctx->fileCount; i++) free(ctx->fileNames[i]);
    free(ctx->fileNames);
    ctx->fileNames = NULL;
    ctx->fileCount = 0;
}

static bool list_dir_and_matching_files(const char *dir, const char *extension, FileBrowseCtx *ctx) {
    free_file_listing(ctx);

    DIR *d = opendir(dir);
    if (!d) return false;

    int dirCap = 0, fileCap = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full[600];
        join_path(full, sizeof(full), dir, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (ctx->dirCount == dirCap) {
                dirCap = dirCap == 0 ? 16 : dirCap * 2;
                char **grown = realloc(ctx->dirNames, sizeof(char *) * dirCap);
                if (!grown) break;
                ctx->dirNames = grown;
            }
            ctx->dirNames[ctx->dirCount] = strdup(entry->d_name);
            if (ctx->dirNames[ctx->dirCount]) ctx->dirCount++;
        } else if (has_extension_ci(entry->d_name, extension)) {
            if (ctx->fileCount == fileCap) {
                fileCap = fileCap == 0 ? 16 : fileCap * 2;
                char **grown = realloc(ctx->fileNames, sizeof(char *) * fileCap);
                if (!grown) break;
                ctx->fileNames = grown;
            }
            ctx->fileNames[ctx->fileCount] = strdup(entry->d_name);
            if (ctx->fileNames[ctx->fileCount]) ctx->fileCount++;
        }
    }
    closedir(d);

    if (ctx->dirCount > 0) qsort(ctx->dirNames, ctx->dirCount, sizeof(char *), compare_names);
    if (ctx->fileCount > 0) qsort(ctx->fileNames, ctx->fileCount, sizeof(char *), compare_names);
    return true;
}

// Menu layout: optionally [0] "Parent folder", then one entry per subfolder
// (navigation only), then one entry per matching file (selectable).
static int file_first_dir_index(const FileBrowseCtx *ctx) {
    return ctx->hasParent ? 1 : 0;
}
static int file_first_file_index(const FileBrowseCtx *ctx) {
    return file_first_dir_index(ctx) + ctx->dirCount;
}

static const char *file_browse_label(int index, void *userdata) {
    FileBrowseCtx *ctx = (FileBrowseCtx *)userdata;
    if (ctx->hasParent && index == 0) return "[.. Parent folder]";
    int firstFile = file_first_file_index(ctx);
    if (index < firstFile) return ctx->dirNames[index - file_first_dir_index(ctx)];
    return ctx->fileNames[index - firstFile];
}

bool sd_browse_pick_file(const char *startDir, const char *extension, char *outPath, size_t outSize) {
    char currentDir[512];
    snprintf(currentDir, sizeof(currentDir), "%s", startDir);

    FileBrowseCtx ctx = {0};
    bool result = false;

    for (;;) {
        ctx.hasParent = !is_sd_root(currentDir);
        if (!list_dir_and_matching_files(currentDir, extension, &ctx)) {
            // Can't open this directory -- back out rather than get stuck.
            // Was silent until now, which looked exactly like the "nothing
            // happens" symptom the double-slash bug above used to cause --
            // say something so a genuine unreadable-folder case isn't
            // mistaken for the picker doing nothing.
            ui_clear();
            ui_print_error("\nCouldn't open that folder.\n");
            ui_wait_for_a();
            break;
        }

        int total = file_first_file_index(&ctx) + ctx.fileCount;
        if (total == 0) {
            // Nothing to navigate into or pick from an empty folder --
            // ui_run_menu requires at least one entry, so bail out as a
            // cancel, but say so first (see the comment above).
            ui_clear();
            ui_print_error("\nNo subfolders or matching files here.\n");
            ui_wait_for_a();
            break;
        }
        int choice = ui_run_menu(currentDir, total, file_browse_label, &ctx);

        if (choice < 0) break; // B: cancel the whole picker

        if (ctx.hasParent && choice == 0) {
            go_to_parent(currentDir);
            continue;
        }

        int firstFile = file_first_file_index(&ctx);
        if (choice < firstFile) {
            const char *sub = ctx.dirNames[choice - file_first_dir_index(&ctx)];
            char next[512];
            join_path(next, sizeof(next), currentDir, sub);
            snprintf(currentDir, sizeof(currentDir), "%s", next);
            continue;
        }

        const char *file = ctx.fileNames[choice - firstFile];
        join_path(outPath, outSize, currentDir, file);
        result = true;
        break;
    }

    free_file_listing(&ctx);
    return result;
}

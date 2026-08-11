#include "ui.h"
#include "http.h"
#include "auth.h"
#include "dropbox.h"
#include "saves.h"
#include "sd_browse.h"
#include "checkpoint_saves.h"
#include "minizip_reader.h"
#include "update_check.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TMP_ZIP_PATH "sdmc:/3ds/Konnect3DS/_tmp_backup.zip"
#define MAX_LOCAL_BACKUPS 64
#define MAX_DROPBOX_BACKUPS_SHOWN 32
#define MAX_CHECKPOINT_TITLE_FOLDERS 128
#define BACKED_UP_MARKS_PATH "sdmc:/3ds/Konnect3DS/backed_up.txt"

// Which titles have been backed up at least once through this app --
// live save, extdata, or a Checkpoint instance, any of them count --
// drives the icon grid's "already backed up" badge (see refresh_visible()
// and ui_set_home_backup_marks()). Persisted as one hex title id per line
// so the badges survive an app restart, not just the current session --
// loaded once at startup (load_backed_up_marks()), appended to on every
// successful upload (mark_backed_up()).
static u64 *s_backedUpIds = NULL;
static u32 s_backedUpCount = 0;
static u32 s_backedUpCapacity = 0;

static bool is_marked_backed_up(u64 titleId) {
    for (u32 i = 0; i < s_backedUpCount; i++) {
        if (s_backedUpIds[i] == titleId) return true;
    }
    return false;
}

static void load_backed_up_marks(void) {
    FILE *f = fopen(BACKED_UP_MARKS_PATH, "r");
    if (!f) return; // no marks yet -- fine, every title just starts unbadged

    char line[32];
    while (fgets(line, sizeof(line), f)) {
        u64 id = strtoull(line, NULL, 16);
        if (id == 0) continue;

        if (s_backedUpCount == s_backedUpCapacity) {
            u32 newCap = s_backedUpCapacity == 0 ? 32 : s_backedUpCapacity * 2;
            u64 *grown = realloc(s_backedUpIds, sizeof(u64) * newCap);
            if (!grown) break; // out of memory; keep what's been loaded so far
            s_backedUpIds = grown;
            s_backedUpCapacity = newCap;
        }
        s_backedUpIds[s_backedUpCount++] = id;
    }
    fclose(f);
}

static void mark_backed_up(u64 titleId) {
    if (is_marked_backed_up(titleId)) return;

    if (s_backedUpCount == s_backedUpCapacity) {
        u32 newCap = s_backedUpCapacity == 0 ? 32 : s_backedUpCapacity * 2;
        u64 *grown = realloc(s_backedUpIds, sizeof(u64) * newCap);
        if (!grown) return; // out of memory; badge just won't show for this one
        s_backedUpIds = grown;
        s_backedUpCapacity = newCap;
    }
    s_backedUpIds[s_backedUpCount++] = titleId;

    FILE *f = fopen(BACKED_UP_MARKS_PATH, "a");
    if (!f) return;
    fprintf(f, "%016llX\n", (unsigned long long)titleId);
    fclose(f);
}

// CHECKPOINT_SAVES_DIR (checkpoint_saves.h) is also used below as the
// folder picker's starting point when it exists, so picking up an
// existing Checkpoint backup by hand is just "open the picker, pick the
// game, confirm" instead of hunting across the whole SD card.

// Shared progress-bar plumbing for every upload/extract call below.
// `label` is the operation's name (e.g. "Uploading to Dropbox",
// "Extracting"); `lastPercent` starts at -1 so the very first callback
// (0%) is never skipped as "unchanged". report_progress() matches both
// HttpProgressFn (http.h) and ZipProgressFn (minizip_reader.h) -- u32 and
// uint32_t are the same underlying type on this platform, so one function
// serves both without a wrapper each. Only redraws when the whole
// percentage actually changes, since the underlying callbacks can fire
// far more often (once per TLS write, once per 4KB zip chunk) than the
// number on screen does.
typedef struct {
    const char *label;
    int lastPercent;
} ProgressState;

static void report_progress(u32 bytesDone, u32 bytesTotal, void *userdata) {
    ProgressState *st = (ProgressState *)userdata;
    int pct = bytesTotal > 0 ? (int)(((u64)bytesDone * 100) / bytesTotal) : 100;
    if (pct == st->lastPercent) return;
    st->lastPercent = pct;
    char msg[64];
    snprintf(msg, sizeof(msg), "%s... %d%%", st->label, pct);
    ui_print_progress(msg);
}

typedef struct {
    InstalledTitle *titles;
    u32 titleCount;
    bool loggedIn;
    // SELECT toggles this (UI_GRID_TOGGLE_EXTDATA) to show only titles
    // with a backable extra-data save instead of every title -- lets
    // e.g. every Pokemon game be found at a glance instead of scrolling
    // past everything else. `visible` holds the indices into `titles`
    // the grid is currently showing (every index, in order, when the
    // filter is off); refresh_visible() rebuilds it after refresh_titles()
    // and whenever the filter is toggled.
    bool extdataFilterOn;
    u32 *visible;
    u32 visibleCount;
    u32 visibleCapacity;
} MenuState;

static const char *title_display_name(const InstalledTitle *t) {
    return t->name[0] ? t->name : (t->productCode[0] ? t->productCode : "Unknown");
}

// ui_run_icon_grid()'s caption callback: index 0/1/2 are its three
// reserved tiles, 3..N+2 line up with title_icon_pixels() below (same
// state->visible indexing, offset by 3 instead of 1). The physical game
// card slot can only ever hold one title at a time and is easy to mix up
// with an SD-installed game sharing a similar name, so it's flagged in
// the caption -- display-only (a static scratch buffer, safe since only
// one caption is ever read per frame): title_display_name() itself is
// left alone since it also feeds Dropbox path building elsewhere, and a
// "(Cartridge)" suffix has no business ending up in a Dropbox folder name.
static const char *grid_label(int index, void *userdata) {
    const MenuState *state = (const MenuState *)userdata;
    if (index == 0) return "Import from SD/3DS folder";
    if (index == 1) return "Checkpoint";
    if (index == 2) return "Other apps...";

    const InstalledTitle *t = &state->titles[state->visible[index - 3]];
    const char *name = title_display_name(t);
    if (t->mediaType != MEDIATYPE_GAME_CARD) return name;

    static char labelBuf[80];
    snprintf(labelBuf, sizeof(labelBuf), "%s (Cartridge)", name);
    return labelBuf;
}

static const u16 *title_icon_pixels(int index, void *userdata) {
    const MenuState *state = (const MenuState *)userdata;
    return state->titles[state->visible[index]].icon;
}

// Rebuilds state->visible from state->titles/titleCount and the current
// extdataFilterOn setting, then hands the result to ui_set_home_icons().
// Called after every refresh_titles() and whenever SELECT toggles the
// filter -- titleCount is also visible's upper bound on how large it'll
// ever need to be, so the backing array only grows, never shrinks.
static void refresh_visible(MenuState *state) {
    if (state->visibleCapacity < state->titleCount) {
        u32 *grown = realloc(state->visible, sizeof(u32) * state->titleCount);
        if (!grown) return; // out of memory; keep showing the previous list
        state->visible = grown;
        state->visibleCapacity = state->titleCount;
    }

    state->visibleCount = 0;
    for (u32 i = 0; i < state->titleCount; i++) {
        if (state->extdataFilterOn && !state->titles[i].extdataAccessible) continue;
        state->visible[state->visibleCount++] = i;
    }

    ui_set_home_icons((int)state->visibleCount, title_icon_pixels, state);

    // Aligned with title_icon_pixels()' own indexing (state->visible[i]) --
    // rebuilt fresh here rather than cached, so it always reflects which
    // titles are visible right now, not whichever were visible the last
    // time the filter changed.
    if (state->visibleCount > 0) {
        bool *marks = malloc(sizeof(bool) * state->visibleCount);
        if (marks) {
            for (u32 i = 0; i < state->visibleCount; i++) {
                marks[i] = is_marked_backed_up(state->titles[state->visible[i]].titleId);
            }
            ui_set_home_backup_marks(marks, (int)state->visibleCount);
            free(marks);
        }
    } else {
        ui_set_home_backup_marks(NULL, 0);
    }
}

static void refresh_titles(MenuState *state) {
    if (state->titles) free(state->titles);
    state->titles = NULL;
    state->titleCount = 0;
    saves_list_titles(&state->titles, &state->titleCount);
    refresh_visible(state);
}

// Best-effort: leaves the header's email blank on failure (network error,
// or somehow not actually logged in) rather than treating it as fatal --
// see dropbox_get_account_email()'s own doc comment.
static void refresh_account_email(DropboxTokens *tokens) {
    char email[128];
    bool ok = dropbox_get_account_email(tokens, email, sizeof(email));
    ui_set_account_email(ok ? email : NULL);
}

static const char *account_menu_label(int index, void *userdata) {
    (void)userdata;
    if (index == 0) return ui_is_email_hidden() ? "Show email in header" : "Hide email in header";
    if (index == 1) return "Log out";
    return "?";
}

// X on the icon grid opens this (see ui_run_icon_grid()'s UI_GRID_ACCOUNT)
// instead of logging out directly -- room to grow (this is where any
// future account-level setting belongs) and a deliberate extra step
// before something as disruptive as logging out. Loops so toggling the
// email visibility doesn't immediately kick back out to the grid.
static void show_account_menu(MenuState *state, DropboxTokens *tokens) {
    for (;;) {
        int choice = ui_run_menu("Manage account", 2, account_menu_label, NULL);
        if (choice < 0) return; // B: back to the grid, nothing changed

        if (choice == 0) {
            ui_toggle_email_visibility();
            continue;
        }

        // choice == 1: log out
        if (ui_confirm("Log out of Dropbox?")) {
            auth_delete_tokens();
            memset(tokens, 0, sizeof(*tokens));
            state->loggedIn = false;
            ui_set_account_email(NULL);
        }
        return;
    }
}

// One entry in show_game_detail()'s local-backup picker: either the
// title's own live save data, or one of Checkpoint's own backup instances
// for it (see checkpoint_saves.h).
typedef struct {
    char label[256];
    bool isLive;
    bool isExtdata; // only meaningful when isLive: extdata instead of the main save
    const CheckpointBackup *checkpoint; // NULL when isLive
} LocalBackupEntry;

// show_game_detail()'s bottom-screen picker has one more option than
// LocalBackupEntry entries: a reserved trailing "download from Dropbox"
// item (index == entryCount) that doesn't back up anything, so its label
// callback needs entryCount alongside the entries themselves.
typedef struct {
    const LocalBackupEntry *entries;
    int entryCount;
} GameDetailMenu;

static const char *game_detail_label(int index, void *userdata) {
    const GameDetailMenu *menu = (const GameDetailMenu *)userdata;
    if (index == menu->entryCount) return "Download a backup from Dropbox...";
    return menu->entries[index].label;
}

static const char *dropbox_entry_label(int index, void *userdata) {
    const DropboxBackupEntry *entries = (const DropboxBackupEntry *)userdata;
    return entries[index].name;
}

// Prints the game's header plus whatever's already uploaded to its Dropbox
// folder on the top screen -- the "cloud side" of show_game_detail()'s
// split view, sitting opposite the bottom screen's local-backup picker.
static void print_dropbox_backups(DropboxTokens *tokens, const char *gameName) {
    ui_clear();
    ui_print_header(gameName);

    char folder[192];
    dropbox_build_game_folder(gameName, folder, sizeof(folder));
    ui_printf("Dropbox: %s\n\n", folder);

    static DropboxBackupEntry entries[MAX_DROPBOX_BACKUPS_SHOWN];
    int count = 0;
    char error[256];
    if (!dropbox_list_backups(tokens, gameName, entries, MAX_DROPBOX_BACKUPS_SHOWN, &count,
                               error, sizeof(error))) {
        char msg[288];
        snprintf(msg, sizeof(msg), "Could not check Dropbox: %s\n", error);
        ui_print_error(msg);
    } else if (count == 0) {
        ui_print("No backups uploaded yet.\n");
    } else {
        for (int i = 0; i < count; i++) {
            if (entries[i].size >= 0) {
                ui_printf("%s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
            } else {
                ui_printf("%s\n", entries[i].name);
            }
        }
    }
    ui_flush();
}

// Backs up and uploads one local backup instance (the live save, or one
// Checkpoint folder) under its own Dropbox filename -- see
// dropbox_build_backup_path() -- so repeated uploads for the same game
// accumulate instead of overwriting each other.
static void upload_local_backup(MenuState *state, DropboxTokens *tokens, int titleIndex,
                                 const LocalBackupEntry *entry) {
    InstalledTitle *t = &state->titles[titleIndex];
    const char *gameName = title_display_name(t);

    ui_clear();
    ui_printf("Backing up %s...\n", entry->label);
    ui_flush();

    char label[256];
    Result rc;
    if (entry->isLive) {
        char timestamp[64];
        time_t now = time(NULL);
        struct tm *tmNow = localtime(&now);
        if (tmNow) strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tmNow);
        else snprintf(timestamp, sizeof(timestamp), "backup");
        if (entry->isExtdata) {
            snprintf(label, sizeof(label), "extdata-%s", timestamp);
            rc = saves_backup_title_extdata(t, TMP_ZIP_PATH);
        } else {
            snprintf(label, sizeof(label), "%s", timestamp);
            rc = saves_backup_title(t, TMP_ZIP_PATH);
        }
    } else {
        snprintf(label, sizeof(label), "%s", entry->checkpoint->name);
        rc = saves_backup_folder(entry->checkpoint->fullPath, TMP_ZIP_PATH);
    }

    if (R_FAILED(rc)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "\nFailed to read save data (0x%08lX).\n", (unsigned long)rc);
        ui_print_error(msg);
        ui_wait_for_a();
        return;
    }

    ui_print("Save extracted.\n");
    ui_flush();

    char dropboxPath[192];
    dropbox_build_backup_path(gameName, label, dropboxPath, sizeof(dropboxPath));

    ProgressState progress = { "Uploading to Dropbox", -1 };
    char error[256];
    bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                   report_progress, &progress, error, sizeof(error));

    remove(TMP_ZIP_PATH);

    if (ok) {
        mark_backed_up(t->titleId);
        char msg[224];
        snprintf(msg, sizeof(msg), "\nUploaded to Dropbox: %s\n", dropboxPath);
        ui_print_success(msg);
    } else {
        char msg[288];
        snprintf(msg, sizeof(msg), "\nUpload failed: %s\n", error);
        ui_print_error(msg);
    }
    ui_wait_for_a();
}

// Downloads one of the game's Dropbox backups and extracts it into a new
// Checkpoint-style backup-instance folder under CHECKPOINT_SAVES_DIR --
// "0x%05X <game name>/<label>", the exact layout checkpoint_saves.c reads
// (find_title_folder() matches the "0x%05X " prefix, checkpoint_list_
// backups() lists whatever subfolders sit under it). This app doesn't
// write the save back into the title's live SAVEDATA archive itself: the
// actual restore is one more step the user does inside Checkpoint, which
// already has a trusted, well-tested restore flow -- this just gets the
// backup onto the SD card where Checkpoint will find it.
static void download_and_prepare_restore(MenuState *state, DropboxTokens *tokens, int titleIndex) {
    InstalledTitle *t = &state->titles[titleIndex];
    const char *gameName = title_display_name(t);

    ui_clear();
    ui_printf("Checking Dropbox backups for %s...\n", gameName);
    ui_flush();

    static DropboxBackupEntry dropboxEntries[MAX_DROPBOX_BACKUPS_SHOWN];
    int count = 0;
    char error[256];
    if (!dropbox_list_backups(tokens, gameName, dropboxEntries, MAX_DROPBOX_BACKUPS_SHOWN, &count,
                               error, sizeof(error))) {
        char msg[288];
        snprintf(msg, sizeof(msg), "\nCould not check Dropbox: %s\n", error);
        ui_print_error(msg);
        ui_wait_for_a();
        return;
    }
    if (count == 0) {
        ui_print_error("\nNo backups uploaded yet for this game.\n");
        ui_wait_for_a();
        return;
    }

    int choice = ui_run_menu("Choose a backup to download", count, dropbox_entry_label, dropboxEntries);
    if (choice < 0) return; // B: cancel

    char folder[192];
    dropbox_build_game_folder(gameName, folder, sizeof(folder));
    char dropboxPath[224];
    snprintf(dropboxPath, sizeof(dropboxPath), "%s/%s", folder, dropboxEntries[choice].name);

    ui_clear();
    ui_printf("Downloading %s...\n", dropboxEntries[choice].name);
    ui_flush();

    bool ok = dropbox_download_file(tokens, dropboxPath, TMP_ZIP_PATH, error, sizeof(error));
    if (!ok) {
        char msg[288];
        snprintf(msg, sizeof(msg), "\nDownload failed: %s\n", error);
        ui_print_error(msg);
        ui_wait_for_a();
        return;
    }

    // The picked entry's own filename (already FAT/Dropbox-safe -- it was
    // sanitized before upload) minus dropbox_build_backup_path()'s ".zip",
    // reused as-is for the local backup-instance folder's name.
    char label[192];
    snprintf(label, sizeof(label), "%s", dropboxEntries[choice].name);
    size_t labelLen = strlen(label);
    if (labelLen > 4 && strcmp(label + labelLen - 4, ".zip") == 0) label[labelLen - 4] = '\0';

    // "extdata-" is a prefix only this app's own upload_local_backup()
    // ever adds (see its isExtdata branch) -- every backup this picker can
    // possibly offer was uploaded by this same app, so it reliably tells
    // an extdata backup apart from a save one, routing it into Checkpoint's
    // separate extdata/ tree instead of mixing it into saves/ where
    // Checkpoint would try to restore it as a SAVEDATA archive and fail.
    bool isExtdata = strncmp(label, "extdata-", 8) == 0;
    const char *destRoot = isExtdata ? CHECKPOINT_EXTDATA_DIR : CHECKPOINT_SAVES_DIR;

    char safeGameName[128];
    dropbox_sanitize_name(gameName, safeGameName, sizeof(safeGameName));

    char destDir[512];
    snprintf(destDir, sizeof(destDir), "%s/0x%05X %s/%s", destRoot,
             (unsigned int)((u32)t->titleId >> 8), safeGameName, label);

    ProgressState extractProgress = { "Extracting", -1 };
    ok = zipr_extract_all(TMP_ZIP_PATH, destDir, report_progress, &extractProgress);
    remove(TMP_ZIP_PATH);

    if (ok) {
        char msg[608];
        snprintf(msg, sizeof(msg),
                 "\nReady at:\n%s\n\nOpen Checkpoint and restore from this backup to finish.\n",
                 destDir);
        ui_print_success(msg);
    } else {
        ui_print_error("\nCould not read the downloaded backup (corrupt zip?).\n");
    }
    ui_wait_for_a();
}

// The per-game screen: top shows what's already backed up to Dropbox
// (print_dropbox_backups()), bottom is a picker (ui_run_menu(), same
// Up/Down/A/B as every other menu here) over the title's local backup
// instances -- its own live save data plus any Checkpoint backups found
// for it, plus a trailing "download from Dropbox" option. Picking a local
// instance uploads it; picking the download option fetches and extracts
// one instead. Either way this loops back (refreshing both the Checkpoint
// scan and the Dropbox listing) instead of returning, so several backups
// can be uploaded/downloaded in one visit; B backs out to the title list.
static void show_game_detail(MenuState *state, DropboxTokens *tokens, int titleIndex) {
    InstalledTitle *t = &state->titles[titleIndex];
    const char *gameName = title_display_name(t);

    static CheckpointBackup checkpointBackups[MAX_LOCAL_BACKUPS];
    static LocalBackupEntry entries[MAX_LOCAL_BACKUPS + 1];

    for (;;) {
        int checkpointCount = 0;
        checkpoint_list_backups(t->titleId, checkpointBackups, MAX_LOCAL_BACKUPS, &checkpointCount);

        int entryCount = 0;
        snprintf(entries[entryCount].label, sizeof(entries[entryCount].label), "Current save data (live)");
        entries[entryCount].isLive = true;
        entries[entryCount].isExtdata = false;
        entries[entryCount].checkpoint = NULL;
        entryCount++;
        if (t->extdataAccessible && entryCount < MAX_LOCAL_BACKUPS + 1) {
            snprintf(entries[entryCount].label, sizeof(entries[entryCount].label), "Extra data (Pokedex/photos/etc.)");
            entries[entryCount].isLive = true;
            entries[entryCount].isExtdata = true;
            entries[entryCount].checkpoint = NULL;
            entryCount++;
        }
        for (int i = 0; i < checkpointCount && entryCount < MAX_LOCAL_BACKUPS + 1; i++) {
            snprintf(entries[entryCount].label, sizeof(entries[entryCount].label), "%s", checkpointBackups[i].name);
            entries[entryCount].isLive = false;
            entries[entryCount].isExtdata = false;
            entries[entryCount].checkpoint = &checkpointBackups[i];
            entryCount++;
        }

        print_dropbox_backups(tokens, gameName);

        GameDetailMenu menu = { entries, entryCount };
        int choice = ui_run_menu(gameName, entryCount + 1, game_detail_label, &menu);
        if (choice < 0) return; // B: back to the title list

        if (choice == entryCount) {
            download_and_prepare_restore(state, tokens, titleIndex);
        } else {
            upload_local_backup(state, tokens, titleIndex, &entries[choice]);
        }
    }
}

// Y on the icon grid: backs up and uploads every listed title's current
// live save in one pass, for "about to swap cards / wipe the SD card"
// moments where visiting each game one at a time would be tedious. Only
// the main live save -- not extdata, not existing Checkpoint backup
// instances -- keeping the batch to the one thing every listed title
// always has, same reasoning as upload_local_backup()'s "live" entry.
// Keeps going past a single title's failure (network hiccup, one bad
// save archive) instead of aborting the whole run, and reports which
// ones failed at the end.
static void backup_all_titles(MenuState *state, DropboxTokens *tokens) {
    if (state->titleCount == 0) {
        ui_clear();
        ui_print_error("\nNo games to back up.\n");
        ui_wait_for_a();
        return;
    }

    if (!ui_confirm("Back up every game to Dropbox?")) return;

    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tmNow = localtime(&now);
    if (tmNow) strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tmNow);
    else snprintf(timestamp, sizeof(timestamp), "backup");

    int okCount = 0, failCount = 0;
    char failedNames[512] = {0};

    for (u32 i = 0; i < state->titleCount; i++) {
        InstalledTitle *t = &state->titles[i];
        const char *gameName = title_display_name(t);

        ui_clear();
        ui_printf("Backup %lu/%lu: %s...\n",
                   (unsigned long)(i + 1), (unsigned long)state->titleCount, gameName);
        ui_flush();

        Result rc = saves_backup_title(t, TMP_ZIP_PATH);
        bool ok = R_SUCCEEDED(rc);
        if (ok) {
            char dropboxPath[192];
            dropbox_build_backup_path(gameName, timestamp, dropboxPath, sizeof(dropboxPath));
            ProgressState progress = { "Uploading to Dropbox", -1 };
            char error[256];
            ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                      report_progress, &progress, error, sizeof(error));
            remove(TMP_ZIP_PATH);
        }

        if (ok) {
            okCount++;
            mark_backed_up(t->titleId);
        } else {
            failCount++;
            size_t used = strlen(failedNames);
            if (used + strlen(gameName) + 3 < sizeof(failedNames)) {
                if (used > 0) strncat(failedNames, ", ", sizeof(failedNames) - used - 1);
                strncat(failedNames, gameName, sizeof(failedNames) - strlen(failedNames) - 1);
            }
        }
    }

    ui_clear();
    if (failCount == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "\nBacked up %d games.\n", okCount);
        ui_print_success(msg);
    } else {
        char msg[768];
        snprintf(msg, sizeof(msg), "\nBacked up %d games, %d failed:\n%s\n",
                 okCount, failCount, failedNames);
        ui_print_error(msg);
    }
    ui_wait_for_a();
}

// Strips Checkpoint's "0x%05X " title-unique-id prefix (see
// CHECKPOINT_SAVES_DIR's comment above) off a folder's basename, if
// present, so a Checkpoint-sourced folder gets a clean game name on
// Dropbox instead of carrying the hex ID along. Any other folder name
// (from manually browsing elsewhere) is used exactly as picked.
static const char *display_name_for_folder(const char *folderPath) {
    const char *base = strrchr(folderPath, '/');
    base = base ? base + 1 : folderPath;

    if (strncmp(base, "0x", 2) == 0) {
        const char *p = base + 2;
        int hexDigits = 0;
        while (isxdigit((unsigned char)*p)) { p++; hexDigits++; }
        if (hexDigits == 5 && *p == ' ' && p[1] != '\0') {
            return p + 1;
        }
    }
    return base;
}

static void mark_backed_up_by_unique_id(MenuState *state, u32 uniqueId);

static void import_and_upload(MenuState *state, DropboxTokens *tokens) {
    char startDir[64];
    struct stat st;
    if (stat(CHECKPOINT_SAVES_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(startDir, sizeof(startDir), "%s", CHECKPOINT_SAVES_DIR);
    } else {
        snprintf(startDir, sizeof(startDir), "sdmc:/");
    }

    char pickedPath[512];
    if (!sd_browse_pick_folder(startDir, pickedPath, sizeof(pickedPath))) {
        return; // cancelled
    }

    const char *gameName = display_name_for_folder(pickedPath);

    ui_clear();
    ui_printf("Zipping %s...\n", gameName);
    ui_flush();

    Result rc = saves_backup_folder(pickedPath, TMP_ZIP_PATH);
    if (R_FAILED(rc)) {
        ui_print_error("\nCould not read that folder.\n");
        ui_wait_for_a();
        return;
    }

    char dropboxPath[192];
    dropbox_build_game_path(gameName, dropboxPath, sizeof(dropboxPath));

    ProgressState progress = { "Uploading to Dropbox", -1 };
    char error[256];
    bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                   report_progress, &progress, error, sizeof(error));

    remove(TMP_ZIP_PATH);

    if (ok) {
        // If the picked folder was one of Checkpoint's own (its basename
        // starts with the "0x%05X " unique-id prefix -- see
        // CHECKPOINT_SAVES_DIR's comment), mark the matching installed
        // title backed up too, same as picking the same folder via the
        // "Checkpoint" tile instead would.
        const char *base = strrchr(pickedPath, '/');
        base = base ? base + 1 : pickedPath;
        unsigned int parsedId = 0;
        if (sscanf(base, "0x%5X ", &parsedId) == 1) {
            mark_backed_up_by_unique_id(state, parsedId);
        }

        char msg[224];
        snprintf(msg, sizeof(msg), "\nUploaded to Dropbox: %s\n", dropboxPath);
        ui_print_success(msg);
    } else {
        char msg[288];
        snprintf(msg, sizeof(msg), "\nUpload failed: %s\n", error);
        ui_print_error(msg);
    }
    ui_wait_for_a();
}

static const char *checkpoint_folder_menu_label(int index, void *userdata) {
    const CheckpointTitleFolder *folders = (const CheckpointTitleFolder *)userdata;
    const char *base = display_name_for_folder(folders[index].fullPath);
    if (!folders[index].isInsertedCartridge) return base;

    static char labelBuf[300];
    snprintf(labelBuf, sizeof(labelBuf), "%s (cartridge in slot)", base);
    return labelBuf;
}

static const char *checkpoint_instance_menu_label(int index, void *userdata) {
    const CheckpointBackup *instances = (const CheckpointBackup *)userdata;
    return instances[index].name;
}

// If `folder` (a raw Checkpoint save folder discovered by
// checkpoint_list_title_folders(), not necessarily a currently-listed
// InstalledTitle) matches an installed title's own unique id, marks that
// title backed up too -- so the grid badge (see refresh_visible()) picks
// up a manual Checkpoint-folder upload the same as one made from
// show_game_detail(). A no-op if nothing matches (uninstalled title, or
// the folder didn't parse a unique id at all).
static void mark_backed_up_by_unique_id(MenuState *state, u32 uniqueId) {
    if (uniqueId == 0) return;
    for (u32 i = 0; i < state->titleCount; i++) {
        if ((u32)(state->titles[i].titleId >> 8) == uniqueId) {
            mark_backed_up(state->titles[i].titleId);
            return;
        }
    }
}

// Y inside show_checkpoint_folder_detail()'s picker: uploads every local
// backup instance in the folder in one pass instead of one at a time,
// same "keep going past a single failure, report a summary" shape as
// backup_all_titles(). All of them go to the same per-instance Dropbox
// paths a one-at-a-time upload would have used (dropbox_build_backup_path
// keyed by each instance's own name), so this is safe to re-run later
// without duplicating anything already uploaded.
static void backup_all_instances_in_folder(MenuState *state, DropboxTokens *tokens,
                                            const CheckpointTitleFolder *folder, const char *gameName,
                                            const CheckpointBackup *instances, int count) {
    char prompt[256];
    snprintf(prompt, sizeof(prompt), "Back up all %d instances of %s?", count, gameName);
    if (!ui_confirm(prompt)) return;

    int okCount = 0, failCount = 0;
    char failedNames[512] = {0};

    for (int i = 0; i < count; i++) {
        ui_clear();
        ui_printf("Backup %d/%d: %s...\n", i + 1, count, instances[i].name);
        ui_flush();

        Result rc = saves_backup_folder(instances[i].fullPath, TMP_ZIP_PATH);
        bool ok = R_SUCCEEDED(rc);
        if (ok) {
            char dropboxPath[192];
            dropbox_build_backup_path(gameName, instances[i].name, dropboxPath, sizeof(dropboxPath));
            ProgressState progress = { "Uploading to Dropbox", -1 };
            char error[256];
            ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                      report_progress, &progress, error, sizeof(error));
            remove(TMP_ZIP_PATH);
        }

        if (ok) {
            okCount++;
        } else {
            failCount++;
            size_t used = strlen(failedNames);
            if (used + strlen(instances[i].name) + 3 < sizeof(failedNames)) {
                if (used > 0) strncat(failedNames, ", ", sizeof(failedNames) - used - 1);
                strncat(failedNames, instances[i].name, sizeof(failedNames) - strlen(failedNames) - 1);
            }
        }
    }

    if (okCount > 0) mark_backed_up_by_unique_id(state, folder->uniqueId);

    ui_clear();
    if (failCount == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "\nBacked up %d instances.\n", okCount);
        ui_print_success(msg);
    } else {
        char msg[768];
        snprintf(msg, sizeof(msg), "\nBacked up %d instances, %d failed:\n%s\n",
                 okCount, failCount, failedNames);
        ui_print_error(msg);
    }
    ui_wait_for_a();
}

// The picker for one Checkpoint title folder's own backup instances --
// same upload flow as upload_local_backup()'s non-live (Checkpoint
// folder) branch, just sourced from a folder found by browsing rather
// than from an installed title's own scan, so there's no InstalledTitle
// or live-save option here. Y backs up every instance shown here in one
// pass (backup_all_instances_in_folder()) instead of picking one at a time.
static void show_checkpoint_folder_detail(MenuState *state, DropboxTokens *tokens,
                                           const CheckpointTitleFolder *folder) {
    const char *gameName = display_name_for_folder(folder->fullPath);

    static CheckpointBackup instances[MAX_LOCAL_BACKUPS];
    int count = 0;
    checkpoint_list_backups_in_folder(folder->fullPath, instances, MAX_LOCAL_BACKUPS, &count);

    if (count == 0) {
        ui_clear();
        ui_print_error("\nNo backup instances found here yet -- use Checkpoint to create one first.\n");
        ui_wait_for_a();
        return;
    }

    for (;;) {
        print_dropbox_backups(tokens, gameName);

        int choice = ui_run_menu_with_all(gameName, count, checkpoint_instance_menu_label, instances);
        if (choice == UI_MENU_ALL) {
            backup_all_instances_in_folder(state, tokens, folder, gameName, instances, count);
            continue;
        }
        if (choice < 0) return; // B: back to the folder list

        ui_clear();
        ui_printf("Backing up %s...\n", instances[choice].name);
        ui_flush();

        Result rc = saves_backup_folder(instances[choice].fullPath, TMP_ZIP_PATH);
        if (R_FAILED(rc)) {
            ui_print_error("\nCould not read that folder.\n");
            ui_wait_for_a();
            continue;
        }

        char dropboxPath[192];
        dropbox_build_backup_path(gameName, instances[choice].name, dropboxPath, sizeof(dropboxPath));

        ProgressState progress = { "Uploading to Dropbox", -1 };
        char error[256];
        bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                       report_progress, &progress, error, sizeof(error));
        remove(TMP_ZIP_PATH);

        if (ok) {
            mark_backed_up_by_unique_id(state, folder->uniqueId);
            char msg[224];
            snprintf(msg, sizeof(msg), "\nUploaded to Dropbox: %s\n", dropboxPath);
            ui_print_success(msg);
        } else {
            char msg[288];
            snprintf(msg, sizeof(msg), "\nUpload failed: %s\n", error);
            ui_print_error(msg);
        }
        ui_wait_for_a();
    }
}

// Tile 1 on the icon grid: browses every folder Checkpoint itself has
// ever saved to CHECKPOINT_SAVES_DIR, not just the ones belonging to a
// title this app's own automatic filtering currently lists -- an
// uninstalled game, or one this app's probe still doesn't recognize for
// some reason, is still reachable here since it only reads the SD card's
// folder structure directly (checkpoint_list_title_folders()), the same
// way Checkpoint itself would show it.
//
// If a physical DS/DSi cartridge is sitting in the game card slot right
// now (saves_detect_ds_card()), its Checkpoint folder is prepended as one
// more entry -- named/pathed exactly the way Checkpoint itself would, so
// picking it works whether or not a backup already exists there. This
// project deliberately never reads the cart's own save chip (see
// saves_detect_ds_card()'s doc comment for why): the actual backup still
// has to happen in Checkpoint, same as every other case this app hands
// off there. Detecting it costs one ROM header read, done fresh every
// time this opens so a cart swapped in after launch is picked up too.
static void show_checkpoint_browser(MenuState *state, DropboxTokens *tokens) {
    static CheckpointTitleFolder folders[MAX_CHECKPOINT_TITLE_FOLDERS + 1];
    int count = 0;
    checkpoint_list_title_folders(folders, MAX_CHECKPOINT_TITLE_FOLDERS, &count);

    char cardGameCode[8], cardDescription[64];
    if (saves_detect_ds_card(cardGameCode, sizeof(cardGameCode), cardDescription, sizeof(cardDescription))) {
        char cardPath[512];
        snprintf(cardPath, sizeof(cardPath), "%s/%s %s", CHECKPOINT_SAVES_DIR, cardGameCode, cardDescription);

        // If this cart's already been backed up via Checkpoint before, the
        // scan above already found its folder -- tag that existing entry
        // instead of listing the same folder twice.
        bool alreadyListed = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(folders[i].fullPath, cardPath) == 0) {
                folders[i].isInsertedCartridge = true;
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed) {
            CheckpointTitleFolder *f = &folders[count++];
            snprintf(f->name, sizeof(f->name), "%s %s", cardGameCode, cardDescription);
            snprintf(f->fullPath, sizeof(f->fullPath), "%s", cardPath);
            f->uniqueId = 0;
            f->isInsertedCartridge = true;
        }
    }

    if (count == 0) {
        ui_clear();
        ui_print_error("\nCheckpoint's saves folder wasn't found -- "
                        "Checkpoint has never backed anything up on this SD card yet.\n");
        ui_wait_for_a();
        return;
    }

    for (;;) {
        int choice = ui_run_menu("Checkpoint backups", count, checkpoint_folder_menu_label, folders);
        if (choice < 0) return; // B: back to the home screen

        show_checkpoint_folder_detail(state, tokens, &folders[choice]);
    }
}

// Tile 2 on the icon grid: a manual fallback for a title
// saves_list_titles()'s automatic filtering still doesn't show for some
// reason -- lists literally every application-category title installed,
// unfiltered (saves_list_all_titles()), and lets the user try backing up
// any of them directly. Expected to include plenty of built-in apps with
// no real save data (Mii Maker, Face Raiders, ...) -- picking one of
// those just fails cleanly with a clear error, same as any other title
// whose save this app can't read.
static const char *other_title_menu_label(int index, void *userdata) {
    const InstalledTitle *titles = (const InstalledTitle *)userdata;
    return title_display_name(&titles[index]);
}

static void show_other_apps_picker(DropboxTokens *tokens) {
    InstalledTitle *titles = NULL;
    u32 count = 0;
    Result rc = saves_list_all_titles(&titles, &count);
    if (R_FAILED(rc) || count == 0) {
        ui_clear();
        ui_print_error("\nNo installed apps found.\n");
        ui_wait_for_a();
        if (titles) free(titles);
        return;
    }

    for (;;) {
        int choice = ui_run_menu("Other apps", (int)count, other_title_menu_label, titles);
        if (choice < 0) break; // B: back to the home screen

        InstalledTitle *t = &titles[choice];
        const char *gameName = title_display_name(t);

        ui_clear();
        ui_printf("Backing up %s...\n", gameName);
        ui_flush();

        Result backupRc = saves_backup_title(t, TMP_ZIP_PATH);
        if (R_FAILED(backupRc)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "\nCould not read this app's save data (0x%08lX).\n", (unsigned long)backupRc);
            ui_print_error(msg);
            ui_wait_for_a();
            continue;
        }

        char timestamp[64];
        time_t now = time(NULL);
        struct tm *tmNow = localtime(&now);
        if (tmNow) strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tmNow);
        else snprintf(timestamp, sizeof(timestamp), "backup");

        char dropboxPath[192];
        dropbox_build_backup_path(gameName, timestamp, dropboxPath, sizeof(dropboxPath));

        ProgressState progress = { "Uploading to Dropbox", -1 };
        char error[256];
        bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath,
                                       report_progress, &progress, error, sizeof(error));
        remove(TMP_ZIP_PATH);

        if (ok) {
            mark_backed_up(t->titleId);
            char msg[224];
            snprintf(msg, sizeof(msg), "\nUploaded to Dropbox: %s\n", dropboxPath);
            ui_print_success(msg);
        } else {
            char msg[288];
            snprintf(msg, sizeof(msg), "\nUpload failed: %s\n", error);
            ui_print_error(msg);
        }
        ui_wait_for_a();
    }

    free(titles);
}

// Temporary diagnostic: confirm on real hardware that the mbedTLS-over-
// sockets rewrite of http.c (replacing the 3DS system httpc/TLS stack,
// which cannot handshake with any modern server -- see
// romfs/certs/README.md) actually completes a TLS connection, and shows
// something on screen while it's blocked doing so instead of looking
// frozen. Called against two different targets from main() below: see
// the comment there for why. Remove once confirmed working.
static void selftest_https(const char *label, const char *url) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Testing connection to %s...\n", label);
    ui_print(msg);
    ui_flush();

    HttpResponse testResp;
    Result testRc = http_request(HTTPC_METHOD_GET, url, NULL, 0, NULL, 0, &testResp);
    if (R_FAILED(testRc)) {
        snprintf(msg, sizeof(msg), "[selftest] %s: FAIL rc=0x%08lX\n", label, (unsigned long)testRc);
        ui_print_error(msg);
        snprintf(msg, sizeof(msg), "  ip=%s tls=%s %s\n", http_get_last_resolved_ip(),
                 http_get_last_tls_version(), http_get_last_tls_cipher());
        ui_print(msg);
        snprintf(msg, sizeof(msg), "  verify=%s\n", http_get_last_verify_info());
        ui_print(msg);
        snprintf(msg, sizeof(msg), "  verify flags: raw=0x%08lX after_mask=0x%08lX\n",
                 (unsigned long)http_get_last_verify_flags_raw(),
                 (unsigned long)http_get_last_verify_flags_after_mask());
        ui_print(msg);
        snprintf(msg, sizeof(msg), "  sent %d bytes, got %d bytes back\n",
                 http_get_last_request_bytes_sent(), http_get_last_response_bytes_received());
        ui_print(msg);
    } else {
        snprintf(msg, sizeof(msg), "[selftest] %s: OK HTTP %lu\n", label, (unsigned long)testResp.status_code);
        ui_print_success(msg);
        http_response_free(&testResp);
    }
}

#ifndef KONNECT3DS_GIT_HASH
#define KONNECT3DS_GIT_HASH "unknown"
#endif

// Runs the startup diagnostics (build hash, system time, HTTPS self-test)
// into the plain scrolling log -- only meaningful once, right after the
// app actually starts talking to the network for the first time, not on
// every logout/login cycle back to the login gate.
static void run_startup_diagnostics(void) {
    ui_clear();
    ui_print_header("Konnect3DS - back up game saves to Dropbox");
    ui_printf("build %s\n", KONNECT3DS_GIT_HASH);
    {
        // TLS certificate validation checks the cert's NotBefore/NotAfter
        // against libc's time(), not the console's System Settings clock
        // display directly -- printing what time() actually returns
        // rules that mismatch in or out instead of trusting the two are
        // the same thing.
        time_t now = time(NULL);
        struct tm *tmNow = localtime(&now);
        char timeBuf[48];
        if (tmNow) {
            strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tmNow);
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "(localtime failed)");
        }
        ui_printf("system time: %s (raw=%lld)\n", timeBuf, (long long)now);
    }
    ui_flush();

    // PKSM-style update notice: best-effort, never blocks startup on a
    // slow/failed GitHub request beyond this screen's own auto-continue
    // timer below -- see update_check_run()'s doc comment for why a
    // failed check isn't reported as "up to date".
    {
        bool hasUpdate = false;
        char latestShort[8] = {0};
        if (update_check_run(&hasUpdate, latestShort, sizeof(latestShort))) {
            if (hasUpdate) {
                char msg[96];
                snprintf(msg, sizeof(msg), "update available: %s (you have %.7s) -- git pull + rebuild\n",
                         latestShort, KONNECT3DS_GIT_HASH);
                ui_print_success(msg);
            } else {
                ui_print("up to date.\n");
            }
        } else {
            ui_print("update check: couldn't reach GitHub (offline?)\n");
        }
        ui_flush();
    }

    // api.dropboxapi.com is the domain the app actually talks to (token
    // exchange/refresh) -- www.dropbox.com only ever opens in the phone's
    // browser via the QR code, this app never fetches it, and the OAuth
    // relay (cloudflare-relay/) is never contacted by the 3DS at all
    // (see include/auth.h). example.com is a control target: if it and
    // api.dropboxapi.com behave differently, that points at something
    // specific to one side rather than a general bug in this app's TLS
    // client.
    selftest_https("api.dropboxapi.com", "https://api.dropboxapi.com/");
    selftest_https("example.com (control)", "https://example.com/");
    ui_wait_briefly(180); // ~3s at 60fps, or skip immediately with A/B/START
}

int main(void) {
    // romfsInit() must run before ui_init(): ui_init() loads the
    // background/folder-icon textures from romfs:/gfx/*.t3x.
    romfsInit();
    ui_init();
    http_init();

    mkdir("sdmc:/3ds", 0777); // may already exist (it's the standard homebrew folder); ignore failure
    mkdir("sdmc:/3ds/Konnect3DS", 0777);
    load_backed_up_marks();

    // First thing drawn after launch -- auth_load_tokens() below is a
    // near-instant local file read, so without this the screen would
    // otherwise sit blank for a moment with nothing to say the app is
    // actually doing something (and not, say, frozen) before either the
    // grid or the login gate shows up.
    ui_clear();
    ui_print("Checking for a saved Dropbox login...\n");
    ui_wait_briefly(30); // ~0.5s, skippable with A/B/START

    DropboxTokens tokens;
    MenuState state = {0};
    state.loggedIn = auth_load_tokens(&tokens);

    bool wantExit = false;
    bool firstRun = true;

    // Outer loop: the login gate on one side, the icon-grid home screen on
    // the other. Logging out (X on the grid -> show_account_menu()) drops
    // back to the top of this loop instead of returning from main() --
    // there's nothing useful to show once logged out except the login gate.
    while (!wantExit) {
        while (!state.loggedIn) {
            if (!ui_run_login_gate()) { wantExit = true; break; }
            bool loginWantExit = false;
            state.loggedIn = auth_run_login_flow(&tokens, &loginWantExit);
            if (loginWantExit) { wantExit = true; break; } // START pressed mid-flow
            if (!state.loggedIn) {
                ui_clear();
                ui_print_error("Login cancelled or failed.\n");
                ui_wait_for_a();
            }
        }
        if (wantExit) break;

        // auth_run_login_flow() leaves the bottom screen full of QR/login
        // instructions (still the last thing flushed); refresh_account_email()
        // (a network call) and refresh_titles() (an AM re-scan) both take
        // real time and don't touch the screen themselves, so without this
        // that stale bottom screen would just sit there looking stuck
        // until the icon grid's first frame. Also covers the plain
        // auth_load_tokens() startup path -- same stale-screen risk,
        // cheaper to always clear here than to track which path ran.
        ui_clear();
        ui_clear_bottom();
        ui_print("Loading your account and titles...\n");
        ui_flush();

        refresh_account_email(&tokens);
        refresh_titles(&state);

        if (firstRun) {
            firstRun = false;
            run_startup_diagnostics();
        }

        while (aptMainLoop()) {
            int choice = ui_run_icon_grid(grid_label, &state);

            if (choice == UI_GRID_EXIT) { wantExit = true; break; }
            if (choice == UI_GRID_CANCEL) continue; // B: no-op at the home screen

            if (choice == UI_GRID_ACCOUNT) {
                show_account_menu(&state, &tokens);
                if (!state.loggedIn) break; // logged out: back to the outer loop's login gate
                continue;
            }

            if (choice == UI_GRID_BACKUP_ALL) {
                backup_all_titles(&state, &tokens);
                refresh_titles(&state);
                continue;
            }

            if (choice == UI_GRID_TOGGLE_EXTDATA) {
                state.extdataFilterOn = !state.extdataFilterOn;
                refresh_visible(&state);
                ui_clear();
                ui_print(state.extdataFilterOn
                    ? "Showing only games with extra data.\n"
                    : "Showing all games.\n");
                ui_flush();
                ui_wait_briefly(90);
                continue;
            }

            if (choice == 0) {
                import_and_upload(&state, &tokens);
                refresh_titles(&state);
                continue;
            }

            if (choice == 1) {
                show_checkpoint_browser(&state, &tokens);
                // Doesn't change which titles are installed, so a full
                // refresh_titles() (an AM re-scan) isn't needed -- just
                // rebuild the badge marks in case something was uploaded.
                refresh_visible(&state);
                continue;
            }

            if (choice == 2) {
                show_other_apps_picker(&tokens);
                refresh_visible(&state);
                continue;
            }

            // choice is an index into state.visible (3..visibleCount+2),
            // not state.titles directly -- state.visible[] is what maps
            // back to a real title when the extdata filter (SELECT) has
            // hidden some, offset by 3 for the three reserved tiles above.
            int titleIndex = (int)state.visible[choice - 3];
            show_game_detail(&state, &tokens, titleIndex);
            // Picks up newly-created Checkpoint backups and any title
            // list changes (cart swap, freshly installed game) each time
            // control returns to the home screen, without a manual
            // "refresh" action to remember to use.
            refresh_titles(&state);
        }
        if (!aptMainLoop()) wantExit = true; // system-requested close mid-loop
    }

    if (state.titles) free(state.titles);
    if (state.visible) free(state.visible);
    http_exit();
    ui_exit();
    romfsExit();
    return 0;
}

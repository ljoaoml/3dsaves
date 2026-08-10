#include "ui.h"
#include "http.h"
#include "auth.h"
#include "dropbox.h"
#include "saves.h"
#include "sd_browse.h"
#include "checkpoint_saves.h"
#include "minizip_reader.h"

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

// CHECKPOINT_SAVES_DIR (checkpoint_saves.h) is also used below as the
// folder picker's starting point when it exists, so picking up an
// existing Checkpoint backup by hand is just "open the picker, pick the
// game, confirm" instead of hunting across the whole SD card.

typedef struct {
    InstalledTitle *titles;
    u32 titleCount;
    bool loggedIn;
} MenuState;

static const char *title_display_name(const InstalledTitle *t) {
    return t->name[0] ? t->name : (t->productCode[0] ? t->productCode : "Unknown");
}

// ui_run_icon_grid()'s caption callback: index 0 is always its reserved
// folder/import tile, 1..N line up with title_icon_pixels() below (same
// state->titles indexing, offset by 1). The physical game card slot can
// only ever hold one title at a time and is easy to mix up with an
// SD-installed game sharing a similar name, so it's flagged in the
// caption -- display-only (a static scratch buffer, safe since only one
// caption is ever read per frame): title_display_name() itself is left
// alone since it also feeds Dropbox path building elsewhere, and a
// "(Cartridge)" suffix has no business ending up in a Dropbox folder name.
static const char *grid_label(int index, void *userdata) {
    const MenuState *state = (const MenuState *)userdata;
    if (index == 0) return "Import from SD/3DS folder";

    const InstalledTitle *t = &state->titles[index - 1];
    const char *name = title_display_name(t);
    if (t->mediaType != MEDIATYPE_GAME_CARD) return name;

    static char labelBuf[80];
    snprintf(labelBuf, sizeof(labelBuf), "%s (Cartucho)", name);
    return labelBuf;
}

static const u16 *title_icon_pixels(int index, void *userdata) {
    const MenuState *state = (const MenuState *)userdata;
    return state->titles[index].icon;
}

static void refresh_titles(MenuState *state) {
    if (state->titles) free(state->titles);
    state->titles = NULL;
    state->titleCount = 0;
    saves_list_titles(&state->titles, &state->titleCount);
    ui_set_home_icons((int)state->titleCount, title_icon_pixels, state);
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
    if (index == 0) return ui_is_email_hidden() ? "Mostrar email no header" : "Ocultar email no header";
    if (index == 1) return "Sair da conta (logout)";
    return "?";
}

// X on the icon grid opens this (see ui_run_icon_grid()'s UI_GRID_ACCOUNT)
// instead of logging out directly -- room to grow (this is where any
// future account-level setting belongs) and a deliberate extra step
// before something as disruptive as logging out. Loops so toggling the
// email visibility doesn't immediately kick back out to the grid.
static void show_account_menu(MenuState *state, DropboxTokens *tokens) {
    for (;;) {
        int choice = ui_run_menu("Gerenciar conta", 2, account_menu_label, NULL);
        if (choice < 0) return; // B: back to the grid, nothing changed

        if (choice == 0) {
            ui_toggle_email_visibility();
            continue;
        }

        // choice == 1: log out
        if (ui_confirm("Sair da conta do Dropbox?")) {
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

    ui_print("Save extracted. Uploading to Dropbox...\n");
    ui_flush();

    char dropboxPath[192];
    dropbox_build_backup_path(gameName, label, dropboxPath, sizeof(dropboxPath));

    char error[256];
    bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath, error, sizeof(error));

    remove(TMP_ZIP_PATH);

    if (ok) {
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

    ui_print("Extracting...\n");
    ui_flush();

    ok = zipr_extract_all(TMP_ZIP_PATH, destDir);
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
        ui_print_error("\nNenhum jogo para fazer backup.\n");
        ui_wait_for_a();
        return;
    }

    if (!ui_confirm("Fazer backup de todos os jogos para o Dropbox?")) return;

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
            char error[256];
            ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath, error, sizeof(error));
            remove(TMP_ZIP_PATH);
        }

        if (ok) {
            okCount++;
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
        snprintf(msg, sizeof(msg), "\nBackup de %d jogos concluido.\n", okCount);
        ui_print_success(msg);
    } else {
        char msg[768];
        snprintf(msg, sizeof(msg), "\nBackup de %d jogos concluido, %d falharam:\n%s\n",
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

static void import_and_upload(DropboxTokens *tokens) {
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

    ui_print("Uploading to Dropbox...\n");
    ui_flush();

    char dropboxPath[192];
    dropbox_build_game_path(gameName, dropboxPath, sizeof(dropboxPath));

    char error[256];
    bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath, error, sizeof(error));

    remove(TMP_ZIP_PATH);

    if (ok) {
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
            state.loggedIn = auth_run_login_flow(&tokens);
            if (!state.loggedIn) {
                ui_clear();
                ui_print_error("Login cancelled or failed.\n");
                ui_wait_for_a();
            }
        }
        if (wantExit) break;

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

            if (choice == 0) {
                import_and_upload(&tokens);
                refresh_titles(&state);
                continue;
            }

            int titleIndex = choice - 1;
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
    http_exit();
    ui_exit();
    romfsExit();
    return 0;
}

#include "ui.h"
#include "http.h"
#include "auth.h"
#include "dropbox.h"
#include "saves.h"
#include "sd_browse.h"
#include "checkpoint_saves.h"

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

static const char *menu_label(int index, void *userdata) {
    MenuState *state = (MenuState *)userdata;
    static char buf[64];

    if (index == 0) {
        return state->loggedIn ? "Log out of Dropbox" : "Log in to Dropbox";
    }
    if (index == 1) return "Refresh title list";
    if (index == 2) return "Import save from folder...";
    if (index == 3) return "Exit";

    int titleIndex = index - 4;
    InstalledTitle *t = &state->titles[titleIndex];
    const char *mediaLabel = t->mediaType == MEDIATYPE_SD ? "SD" : "Cart";
    if (t->name[0]) {
        snprintf(buf, sizeof(buf), "%s (%s)", t->name, mediaLabel);
    } else {
        snprintf(buf, sizeof(buf), "%s (%s) [%016llX]",
                 t->productCode[0] ? t->productCode : "????",
                 mediaLabel, (unsigned long long)t->titleId);
    }
    return buf;
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

static const char *title_display_name(const InstalledTitle *t) {
    return t->name[0] ? t->name : (t->productCode[0] ? t->productCode : "Unknown");
}

// One entry in show_game_detail()'s local-backup picker: either the
// title's own live save data, or one of Checkpoint's own backup instances
// for it (see checkpoint_saves.h).
typedef struct {
    char label[256];
    bool isLive;
    const CheckpointBackup *checkpoint; // NULL when isLive
} LocalBackupEntry;

static const char *local_backup_label(int index, void *userdata) {
    const LocalBackupEntry *entries = (const LocalBackupEntry *)userdata;
    return entries[index].label;
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
        time_t now = time(NULL);
        struct tm *tmNow = localtime(&now);
        if (tmNow) strftime(label, sizeof(label), "%Y%m%d-%H%M%S", tmNow);
        else snprintf(label, sizeof(label), "backup");
        rc = saves_backup_title(t, TMP_ZIP_PATH);
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

// The per-game screen: top shows what's already backed up to Dropbox
// (print_dropbox_backups()), bottom is a picker (ui_run_menu(), same
// Up/Down/A/B as every other menu here) over the title's local backup
// instances -- its own live save data plus any Checkpoint backups found
// for it. Picking one uploads it and loops back (refreshing both the
// Checkpoint scan and the Dropbox listing) instead of returning, so
// several backups can be uploaded in one visit; B backs out to the title
// list.
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
        entries[entryCount].checkpoint = NULL;
        entryCount++;
        for (int i = 0; i < checkpointCount && entryCount < MAX_LOCAL_BACKUPS + 1; i++) {
            snprintf(entries[entryCount].label, sizeof(entries[entryCount].label), "%s", checkpointBackups[i].name);
            entries[entryCount].isLive = false;
            entries[entryCount].checkpoint = &checkpointBackups[i];
            entryCount++;
        }

        print_dropbox_backups(tokens, gameName);

        int choice = ui_run_menu(gameName, entryCount, local_backup_label, entries);
        if (choice < 0) return; // B: back to the title list

        upload_local_backup(state, tokens, titleIndex, &entries[choice]);
    }
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

    refresh_titles(&state);

    ui_clear();
    ui_print_header("Konnect3DS - back up game saves to Dropbox");
#ifndef KONNECT3DS_GIT_HASH
#define KONNECT3DS_GIT_HASH "unknown"
#endif
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

    ui_print("Select a title on the bottom screen, or\n");
    ui_print("log in to Dropbox first if you haven't yet.\n");
    ui_flush();

    while (aptMainLoop()) {
        int total = 4 + (int)state.titleCount;
        int choice = ui_run_menu("Konnect3DS", total, menu_label, &state);

        if (choice < 0) break; // B on the top-level menu = quit

        if (choice == 0) {
            if (state.loggedIn) {
                if (ui_confirm("Log out of Dropbox?")) {
                    auth_delete_tokens();
                    memset(&tokens, 0, sizeof(tokens));
                    state.loggedIn = false;
                    ui_print_success("\nLogged out.\n");
                    ui_flush();
                }
            } else {
                state.loggedIn = auth_run_login_flow(&tokens);
                ui_clear();
                if (state.loggedIn) {
                    ui_print_success("Logged in to Dropbox.\n");
                } else {
                    ui_print_error("Login cancelled or failed.\n");
                }
                ui_flush();
            }
        } else if (choice == 1) {
            refresh_titles(&state);
            ui_clear();
            ui_printf("Found %lu titles.\n", (unsigned long)state.titleCount);
            ui_flush();
        } else if (choice == 2) {
            if (!state.loggedIn) {
                ui_clear();
                ui_print("Log in to Dropbox first.\n");
                ui_flush();
                ui_wait_for_a();
                continue;
            }
            import_and_upload(&tokens);
        } else if (choice == 3) {
            break;
        } else {
            int titleIndex = choice - 4;
            if (!state.loggedIn) {
                ui_clear();
                ui_print("Log in to Dropbox first.\n");
                ui_flush();
                ui_wait_for_a();
                continue;
            }
            show_game_detail(&state, &tokens, titleIndex);
        }
    }

    if (state.titles) free(state.titles);
    http_exit();
    ui_exit();
    romfsExit();
    return 0;
}

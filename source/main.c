#include "ui.h"
#include "http.h"
#include "auth.h"
#include "dropbox.h"
#include "saves.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TMP_ZIP_PATH "sdmc:/3dsaves/_tmp_backup.zip"

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
    if (index == 2) return "Exit";

    int titleIndex = index - 3;
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

static void refresh_titles(MenuState *state) {
    if (state->titles) free(state->titles);
    state->titles = NULL;
    state->titleCount = 0;
    saves_list_titles(&state->titles, &state->titleCount);
}

static void backup_and_upload(MenuState *state, DropboxTokens *tokens, int titleIndex) {
    InstalledTitle *t = &state->titles[titleIndex];

    ui_clear();
    ui_printf("Backing up %s...\n",
              t->name[0] ? t->name : (t->productCode[0] ? t->productCode : "unknown title"));
    ui_flush();

    Result rc = saves_backup_title(t, TMP_ZIP_PATH);
    if (R_FAILED(rc)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "\nFailed to read save data (0x%08lX).\n", (unsigned long)rc);
        ui_print_error(msg);
        ui_print("Does this title have any save data yet?\n");
        ui_wait_for_a();
        return;
    }

    ui_print("Save extracted. Uploading to Dropbox...\n");
    ui_flush();

    char dropboxPath[128];
    snprintf(dropboxPath, sizeof(dropboxPath), "/3dsaves/%s_%016llX.zip",
              t->productCode[0] ? t->productCode : "UNKNOWN",
              (unsigned long long)t->titleId);

    char error[256];
    bool ok = dropbox_upload_file(tokens, TMP_ZIP_PATH, dropboxPath, error, sizeof(error));

    remove(TMP_ZIP_PATH);

    if (ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), "\nUploaded to Dropbox: %s\n", dropboxPath);
        ui_print_success(msg);
    } else {
        char msg[288];
        snprintf(msg, sizeof(msg), "\nUpload failed: %s\n", error);
        ui_print_error(msg);
    }
    ui_wait_for_a();
}

int main(void) {
    ui_init();
    romfsInit();
    http_init();

    mkdir("sdmc:/3dsaves", 0777);

    DropboxTokens tokens;
    MenuState state = {0};
    state.loggedIn = auth_load_tokens(&tokens);

    refresh_titles(&state);

    ui_clear();
    ui_print_header("3dsaves - back up game saves to Dropbox");
    ui_print("Select a title on the bottom screen, or\n");
    ui_print("log in to Dropbox first if you haven't yet.\n");
    ui_flush();

    while (aptMainLoop()) {
        int total = 3 + (int)state.titleCount;
        int choice = ui_run_menu("3dsaves", total, menu_label, &state);

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
            break;
        } else {
            int titleIndex = choice - 3;
            if (!state.loggedIn) {
                ui_clear();
                ui_print("Log in to Dropbox first.\n");
                ui_flush();
                ui_wait_for_a();
                continue;
            }
            backup_and_upload(&state, &tokens, titleIndex);
        }
    }

    if (state.titles) free(state.titles);
    http_exit();
    romfsExit();
    ui_exit();
    return 0;
}

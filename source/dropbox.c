#include "dropbox.h"
#include "http.h"
#include "minijson.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DROPBOX_UPLOAD_URL "https://content.dropboxapi.com/2/files/upload"
#define DROPBOX_LIST_FOLDER_URL "https://api.dropboxapi.com/2/files/list_folder"

// Dropbox rejects '/','\','<','>',':','"','|','?','*' and other control
// characters in a path component (it also syncs to Windows clients, which
// is where most of that list comes from), and a trailing space or period.
// Game titles routinely contain some of these (e.g. "The Legend of
// Zelda: Ocarina of Time 3D"), so this can't just be inlined into a path
// the way the old product-code-only naming could.
static void dropbox_sanitize_name(const char *name, char *out, size_t outSize) {
    if (outSize == 0) return;
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < outSize; p++) {
        unsigned char c = (unsigned char)*p;
        bool forbidden = c < 0x20 || c == 0x7F ||
            c == '/' || c == '\\' || c == '<' || c == '>' || c == ':' ||
            c == '"' || c == '|' || c == '?' || c == '*';
        out[o++] = forbidden ? '_' : (char)c;
    }
    out[o] = '\0';

    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = '\0';
    if (out[0] == '\0') snprintf(out, outSize, "Unknown");
}

void dropbox_build_game_path(const char *name, char *out, size_t outSize) {
    char safe[128];
    dropbox_sanitize_name(name, safe, sizeof(safe));
    snprintf(out, outSize, "/Konnect3DS/%s/%s.zip", safe, safe);
}

void dropbox_build_game_folder(const char *name, char *out, size_t outSize) {
    char safe[128];
    dropbox_sanitize_name(name, safe, sizeof(safe));
    snprintf(out, outSize, "/Konnect3DS/%s", safe);
}

void dropbox_build_backup_path(const char *name, const char *label, char *out, size_t outSize) {
    char safeName[128];
    char safeLabel[128];
    dropbox_sanitize_name(name, safeName, sizeof(safeName));
    dropbox_sanitize_name(label, safeLabel, sizeof(safeLabel));
    snprintf(out, outSize, "/Konnect3DS/%s/%s.zip", safeName, safeLabel);
}

bool dropbox_upload_file(DropboxTokens *tokens, const char *localPath,
                          const char *dropboxPath, char *errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) errorOut[0] = '\0';

    if (!auth_ensure_valid(tokens, false)) {
        if (errorOut) snprintf(errorOut, errorOutSize, "not logged in / token refresh failed");
        return false;
    }

    FILE *f = fopen(localPath, "rb");
    if (!f) {
        if (errorOut) snprintf(errorOut, errorOutSize, "could not open local file");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        if (errorOut) snprintf(errorOut, errorOutSize, "could not stat local file");
        return false;
    }

    // Dropbox-API-Arg is a JSON object describing the upload; dropboxPath
    // is either built by dropbox_build_game_path() above (which already
    // strips '"' and other JSON/path-breaking characters via
    // dropbox_sanitize_name()) or passed in directly by a caller that
    // controls it entirely, so it's safe to inline here.
    char apiArg[512];
    snprintf(apiArg, sizeof(apiArg),
             "{\"path\":\"%s\",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true}",
             dropboxPath);

    HttpResponse resp;
    Result rc;
    bool retried = false;

    for (;;) {
        char authHeader[sizeof(tokens->access_token) + 16]; // "Bearer " + token
        snprintf(authHeader, sizeof(authHeader), "Bearer %s", tokens->access_token);
        HttpHeader headers[] = {
            {"Authorization", authHeader},
            {"Dropbox-API-Arg", apiArg},
            {"Content-Type", "application/octet-stream"},
        };

        rc = http_request_file_body(HTTPC_METHOD_POST, DROPBOX_UPLOAD_URL,
                                     headers, 3, f, (u32)size, &resp);
        if (R_FAILED(rc)) {
            fclose(f);
            if (errorOut) snprintf(errorOut, errorOutSize, "network error (0x%08lX)", (unsigned long)rc);
            return false;
        }

        // expires_at_unix is compared against the 3DS's own clock, which
        // isn't reliably UTC (see main.c's startup diagnostic) -- rather
        // than trust that comparison alone, treat any 401 (Dropbox uses
        // both "invalid_access_token" and "expired_access_token" for
        // this) as the authoritative signal: force a real refresh and
        // retry exactly once before giving up.
        if (resp.status_code == 401 && !retried) {
            http_response_free(&resp);
            if (auth_ensure_valid(tokens, true)) {
                retried = true;
                continue;
            }
            fclose(f);
            if (errorOut) snprintf(errorOut, errorOutSize, "login expired -- log in again");
            return false;
        }

        break;
    }

    fclose(f);

    bool ok = (resp.status_code == 200);
    if (!ok && errorOut && resp.body.data) {
        char summary[256] = {0};
        if (json_get_string((const char *)resp.body.data, "error_summary", summary, sizeof(summary))) {
            snprintf(errorOut, errorOutSize, "%s", summary);
        } else {
            snprintf(errorOut, errorOutSize, "HTTP %lu", (unsigned long)resp.status_code);
        }
    }

    http_response_free(&resp);
    return ok;
}

bool dropbox_list_backups(DropboxTokens *tokens, const char *gameName,
                           DropboxBackupEntry *out, int maxEntries, int *outCount,
                           char *errorOut, size_t errorOutSize) {
    if (errorOut && errorOutSize > 0) errorOut[0] = '\0';
    if (outCount) *outCount = 0;

    if (!auth_ensure_valid(tokens, false)) {
        if (errorOut) snprintf(errorOut, errorOutSize, "not logged in / token refresh failed");
        return false;
    }

    char folder[192];
    dropbox_build_game_folder(gameName, folder, sizeof(folder));

    // `folder` came from dropbox_build_game_folder(), which sanitizes via
    // dropbox_sanitize_name() -- already stripped of '"' and other
    // JSON/path-breaking characters, so safe to inline like
    // dropbox_upload_file()'s apiArg already does.
    char apiArg[256];
    snprintf(apiArg, sizeof(apiArg), "{\"path\":\"%s\"}", folder);

    HttpResponse resp;
    Result rc;
    bool retried = false;

    for (;;) {
        char authHeader[sizeof(tokens->access_token) + 16]; // "Bearer " + token
        snprintf(authHeader, sizeof(authHeader), "Bearer %s", tokens->access_token);
        HttpHeader headers[] = {
            {"Authorization", authHeader},
            {"Content-Type", "application/json"},
        };

        rc = http_request(HTTPC_METHOD_POST, DROPBOX_LIST_FOLDER_URL, headers, 2,
                           (const u8 *)apiArg, (u32)strlen(apiArg), &resp);
        if (R_FAILED(rc)) {
            if (errorOut) snprintf(errorOut, errorOutSize, "network error (0x%08lX)", (unsigned long)rc);
            return false;
        }

        // Same 401-retry contract as dropbox_upload_file() above.
        if (resp.status_code == 401 && !retried) {
            http_response_free(&resp);
            if (auth_ensure_valid(tokens, true)) {
                retried = true;
                continue;
            }
            if (errorOut) snprintf(errorOut, errorOutSize, "login expired -- log in again");
            return false;
        }

        break;
    }

    if (resp.status_code != 200) {
        char summary[256] = {0};
        bool gotSummary = resp.body.data &&
            json_get_string((const char *)resp.body.data, "error_summary", summary, sizeof(summary));

        // A game folder that's never had a backup uploaded to it yet is a
        // normal "nothing here" state for this app (every game starts
        // with zero cloud backups), not a failure -- Dropbox reports it as
        // a 409 with an error_summary starting "path/not_found/...".
        if (gotSummary && strstr(summary, "not_found")) {
            http_response_free(&resp);
            return true; // *outCount is already 0
        }

        if (errorOut) {
            if (gotSummary) snprintf(errorOut, errorOutSize, "%s", summary);
            else snprintf(errorOut, errorOutSize, "HTTP %lu", (unsigned long)resp.status_code);
        }
        http_response_free(&resp);
        return false;
    }

    const char *json = (const char *)resp.body.data;
    const char *arrayEnd;
    const char *cursor = json ? json_find_array(json, "entries", &arrayEnd) : NULL;

    int count = 0;
    char entryJson[512];
    while (cursor && count < maxEntries &&
           json_array_next_object(&cursor, arrayEnd, entryJson, sizeof(entryJson))) {
        char tag[16] = {0};
        json_get_string(entryJson, ".tag", tag, sizeof(tag));
        if (strcmp(tag, "file") != 0) continue; // skip subfolders -- shouldn't normally exist here

        DropboxBackupEntry *e = &out[count];
        if (!json_get_string(entryJson, "name", e->name, sizeof(e->name))) continue;
        e->size = -1;
        json_get_int(entryJson, "size", &e->size);
        count++;
    }

    if (outCount) *outCount = count;
    http_response_free(&resp);
    return true;
}

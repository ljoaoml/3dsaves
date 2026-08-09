#include "dropbox.h"
#include "http.h"
#include "minijson.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DROPBOX_UPLOAD_URL "https://content.dropboxapi.com/2/files/upload"

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

    // Dropbox-API-Arg is a JSON object describing the upload; dropboxPath is
    // built from a product code (ASCII, no quotes) so it's safe to inline.
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

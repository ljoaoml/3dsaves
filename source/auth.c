#include "auth.h"
#include "http.h"
#include "sha256.h"
#include "base64url.h"
#include "minijson.h"
#include "swkbd_util.h"
#include "ui.h"
#include "qr_display.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define DROPBOX_AUTHORIZE_URL "https://www.dropbox.com/oauth2/authorize"
#define DROPBOX_TOKEN_URL "https://api.dropboxapi.com/oauth2/token"

static bool random_bytes(void *out, size_t size) {
    Result rc = psInit();
    if (R_FAILED(rc)) return false;
    rc = PS_GenerateRandomBytes(out, size);
    psExit();
    return R_SUCCEEDED(rc);
}

static void make_pkce_pair(char verifier_out[128], char challenge_out[128]) {
    uint8_t randomBytes[32];
    if (!random_bytes(randomBytes, sizeof(randomBytes))) {
        // Extremely unlikely fallback; still varies per boot via osGetTime.
        srand((unsigned)osGetTime());
        for (size_t i = 0; i < sizeof(randomBytes); i++) randomBytes[i] = (uint8_t)rand();
    }
    base64url_encode(randomBytes, sizeof(randomBytes), verifier_out); // 43 chars

    uint8_t hash[SHA256_BLOCK_SIZE];
    sha256_buf((const uint8_t *)verifier_out, strlen(verifier_out), hash);
    base64url_encode(hash, sizeof(hash), challenge_out); // 43 chars
}

bool auth_load_tokens(DropboxTokens *tokens) {
    memset(tokens, 0, sizeof(*tokens));
    FILE *f = fopen(DROPBOX_TOKEN_FILE, "rb");
    if (!f) return false;

    char line[600];
    bool haveAccess = false, haveRefresh = false;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (strncmp(line, "access_token=", 13) == 0) {
            strncpy(tokens->access_token, line + 13, sizeof(tokens->access_token) - 1);
            haveAccess = true;
        } else if (strncmp(line, "refresh_token=", 14) == 0) {
            strncpy(tokens->refresh_token, line + 14, sizeof(tokens->refresh_token) - 1);
            haveRefresh = true;
        } else if (strncmp(line, "expires_at=", 11) == 0) {
            tokens->expires_at_unix = strtoull(line + 11, NULL, 10);
        }
    }
    fclose(f);
    tokens->valid = haveAccess && haveRefresh;
    return tokens->valid;
}

bool auth_save_tokens(const DropboxTokens *tokens) {
    mkdir("sdmc:/3dsaves", 0777);
    FILE *f = fopen(DROPBOX_TOKEN_FILE, "wb");
    if (!f) return false;
    fprintf(f, "access_token=%s\n", tokens->access_token);
    fprintf(f, "refresh_token=%s\n", tokens->refresh_token);
    fprintf(f, "expires_at=%llu\n", (unsigned long long)tokens->expires_at_unix);
    fclose(f);
    return true;
}

void auth_delete_tokens(void) {
    remove(DROPBOX_TOKEN_FILE);
}

static bool parse_token_response(const char *json, DropboxTokens *tokens, bool keepExistingRefresh) {
    char access[512] = {0};
    if (!json_get_string(json, "access_token", access, sizeof(access))) return false;
    strncpy(tokens->access_token, access, sizeof(tokens->access_token) - 1);

    char refresh[256] = {0};
    if (json_get_string(json, "refresh_token", refresh, sizeof(refresh))) {
        strncpy(tokens->refresh_token, refresh, sizeof(tokens->refresh_token) - 1);
    } else if (!keepExistingRefresh) {
        return false; // first login must return a refresh token
    }

    long expiresIn = 14400; // Dropbox default access token lifetime, seconds
    json_get_int(json, "expires_in", &expiresIn);
    tokens->expires_at_unix = (u64)time(NULL) + (u64)expiresIn - 60; // 60s safety margin

    tokens->valid = true;
    return true;
}

bool auth_run_login_flow(DropboxTokens *out) {
    memset(out, 0, sizeof(*out));

    char verifier[128], challenge[128];
    make_pkce_pair(verifier, challenge);

    char url[768];
    snprintf(url, sizeof(url),
             "%s?client_id=%s&response_type=code&code_challenge=%s"
             "&code_challenge_method=S256&token_access_type=offline",
             DROPBOX_AUTHORIZE_URL, DROPBOX_CLIENT_ID, challenge);

    ui_clear();
    ui_clear_bottom();
    ui_print_header_bottom("Link Dropbox account");
    ui_print_bottom("1. Scan the QR code on the TOP screen\n");
    ui_print_bottom("   with your phone's camera.\n");
    ui_print_bottom("2. Log in and click Allow.\n");
    ui_print_bottom("3. Copy the code Dropbox shows you.\n");
    ui_print_bottom("4. Press A here to type it in.\n\n");
    ui_print_bottom("(B to cancel)\n\n");
    ui_print_bottom("Can't scan? Full URL:\n");
    ui_print_bottom(url);
    ui_print_bottom("\n");
    ui_flush();

    int qrResult = qr_display_and_wait(url);
    if (qrResult == 0) return false; // user pressed B
    if (qrResult == -1) {
        // QR generation failed (shouldn't happen for a normal-length URL) --
        // fall back to the plain-text URL already shown on the bottom
        // screen and let A/B drive the same confirm/cancel choice.
        while (true) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_A) break;
            if (kDown & KEY_B) return false;
            gspWaitForVBlank();
        }
    }

    // qr_display_and_wait drew straight to the top screen's framebuffer,
    // bypassing the console entirely -- clear it properly now so the
    // console's own state (and the framebuffer) are back to normal before
    // we print anything through it again.
    ui_clear();
    ui_flush();

    char code[256] = {0};
    if (!swkbd_get_text("Paste the Dropbox authorization code", code, sizeof(code)) ||
        strlen(code) == 0) {
        return false;
    }

    char encodedCode[512], encodedVerifier[256];
    http_url_encode(code, encodedCode, sizeof(encodedCode));
    http_url_encode(verifier, encodedVerifier, sizeof(encodedVerifier));

    char body[1024];
    int bodyLen = snprintf(body, sizeof(body),
                            "grant_type=authorization_code&code=%s&client_id=%s&code_verifier=%s",
                            encodedCode, DROPBOX_CLIENT_ID, encodedVerifier);

    HttpHeader headers[] = {
        {"Content-Type", "application/x-www-form-urlencoded"},
    };

    ui_print("\nExchanging code for tokens...\n");
    ui_flush();

    HttpResponse resp;
    Result rc = http_request(HTTPC_METHOD_POST, DROPBOX_TOKEN_URL, headers, 1,
                              (const u8 *)body, (u32)bodyLen, &resp);
    if (R_FAILED(rc)) {
        ui_print("Network error contacting Dropbox.\n");
        return false;
    }

    bool ok = false;
    if (resp.status_code == 200 && resp.body.data) {
        ok = parse_token_response((const char *)resp.body.data, out, false);
    }
    if (!ok) {
        ui_print("Login failed. Response:\n");
        if (resp.body.data) ui_print((const char *)resp.body.data);
    }
    http_response_free(&resp);

    if (ok) auth_save_tokens(out);
    return ok;
}

bool auth_ensure_valid(DropboxTokens *tokens) {
    if (!tokens->valid || strlen(tokens->refresh_token) == 0) return false;

    u64 now = (u64)time(NULL);
    if (tokens->expires_at_unix > now) return true; // still fresh

    char body[512];
    int bodyLen = snprintf(body, sizeof(body),
                            "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                            tokens->refresh_token, DROPBOX_CLIENT_ID);

    HttpHeader headers[] = {
        {"Content-Type", "application/x-www-form-urlencoded"},
    };

    HttpResponse resp;
    Result rc = http_request(HTTPC_METHOD_POST, DROPBOX_TOKEN_URL, headers, 1,
                              (const u8 *)body, (u32)bodyLen, &resp);
    if (R_FAILED(rc)) return false;

    bool ok = false;
    if (resp.status_code == 200 && resp.body.data) {
        ok = parse_token_response((const char *)resp.body.data, tokens, true);
    }
    http_response_free(&resp);

    if (ok) auth_save_tokens(tokens);
    return ok;
}

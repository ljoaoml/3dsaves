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

// Unrelated to the PKCE pair -- just a random correlation id for the
// relay (see cloudflare-relay/), so it knows which in-flight login a
// polling request belongs to. Base64url output needs no URL-encoding.
static void make_state_token(char out[64]) {
    uint8_t randomBytes[24];
    if (!random_bytes(randomBytes, sizeof(randomBytes))) {
        srand((unsigned)osGetTime() ^ 0x5A5A5A5A);
        for (size_t i = 0; i < sizeof(randomBytes); i++) randomBytes[i] = (uint8_t)rand();
    }
    base64url_encode(randomBytes, sizeof(randomBytes), out); // 32 chars
}

typedef enum {
    RELAY_PENDING,
    RELAY_READY,
    RELAY_ERROR,
    RELAY_UNAVAILABLE,
} RelayPollResult;

// Asks cloudflare-relay/ whether Dropbox has redirected the code back yet
// for this login attempt (`state`). See that project's README for why
// this exists: the 3DS can't receive an OAuth redirect itself since the
// browser completing the login runs on a different device (the phone).
// diag is filled with a short human-readable reason whenever the result
// is RELAY_UNAVAILABLE, so the caller can show *why* instead of just
// "still waiting" -- this is what actually pinned down the missing-root-CA
// bug that a plain "can't reach relay" message couldn't distinguish from
// a real network hiccup.
static RelayPollResult poll_relay(const char *state, char *codeOut, size_t codeOutSize,
                                   char *errorOut, size_t errorOutSize,
                                   char *diag, size_t diagSize) {
    char url[512];
    snprintf(url, sizeof(url), "%s/poll?state=%s", RELAY_BASE_URL, state);

    HttpResponse resp;
    Result rc = http_request(HTTPC_METHOD_GET, url, NULL, 0, NULL, 0, &resp);
    if (R_FAILED(rc)) {
        if (diag) snprintf(diag, diagSize, "rc=0x%08lX", (unsigned long)rc);
        return RELAY_UNAVAILABLE;
    }

    RelayPollResult result = RELAY_UNAVAILABLE;
    if (resp.status_code == 200 && resp.body.data) {
        char status[16] = {0};
        const char *json = (const char *)resp.body.data;
        if (json_get_string(json, "status", status, sizeof(status))) {
            if (strcmp(status, "ready") == 0 && json_get_string(json, "code", codeOut, codeOutSize)) {
                result = RELAY_READY;
            } else if (strcmp(status, "pending") == 0) {
                result = RELAY_PENDING;
            } else if (strcmp(status, "error") == 0) {
                json_get_string(json, "error", errorOut, errorOutSize);
                result = RELAY_ERROR;
            } else if (diag) {
                snprintf(diag, diagSize, "unexpected status field");
            }
        } else if (diag) {
            snprintf(diag, diagSize, "bad JSON, HTTP %lu", (unsigned long)resp.status_code);
        }
    } else if (diag) {
        snprintf(diag, diagSize, "HTTP %lu", (unsigned long)resp.status_code);
    }
    http_response_free(&resp);
    return result;
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
    mkdir("sdmc:/3ds", 0777); // may already exist (it's the standard homebrew folder); ignore failure
    mkdir("sdmc:/3ds/Konnect3DS", 0777);
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

    // If a relay (cloudflare-relay/) is configured, use a real redirect_uri
    // so the login can complete without the user typing anything on the
    // 3DS -- see include/auth.h and cloudflare-relay/README.md. Otherwise
    // fall back to the original no-redirect-URI flow (manual code entry).
    bool useRelay = strcmp(RELAY_BASE_URL, "PUT_YOUR_RELAY_URL_HERE") != 0;
    char state[64] = {0};

    char url[1024];
    if (useRelay) {
        make_state_token(state);
        char redirectUri[256];
        snprintf(redirectUri, sizeof(redirectUri), "%s/callback", RELAY_BASE_URL);
        char encodedRedirectUri[512];
        http_url_encode(redirectUri, encodedRedirectUri, sizeof(encodedRedirectUri));

        snprintf(url, sizeof(url),
                 "%s?client_id=%s&response_type=code&code_challenge=%s"
                 "&code_challenge_method=S256&token_access_type=offline"
                 "&redirect_uri=%s&state=%s",
                 DROPBOX_AUTHORIZE_URL, DROPBOX_CLIENT_ID, challenge,
                 encodedRedirectUri, state);
    } else {
        snprintf(url, sizeof(url),
                 "%s?client_id=%s&response_type=code&code_challenge=%s"
                 "&code_challenge_method=S256&token_access_type=offline",
                 DROPBOX_AUTHORIZE_URL, DROPBOX_CLIENT_ID, challenge);
    }

    ui_clear();
    ui_clear_bottom();
    ui_print_header_bottom("Link Dropbox account");
#ifndef KONNECT3DS_GIT_HASH
#define KONNECT3DS_GIT_HASH "unknown"
#endif
    // The top screen's build-hash line (main.c) gets overwritten by the
    // QR code the moment this screen shows, so it and any diagnostic
    // printed later on THIS screen would never appear in the same photo
    // -- repeat it here so a single screenshot always has both.
    {
        char buildMsg[32];
        snprintf(buildMsg, sizeof(buildMsg), "build %s\n", KONNECT3DS_GIT_HASH);
        ui_print_bottom(buildMsg);
    }
    ui_print_bottom("1. Scan the QR code on the TOP screen\n");
    ui_print_bottom("   with your phone's camera.\n");
    ui_print_bottom("2. Log in and click Allow.\n");
    if (useRelay) {
        ui_print_bottom("3. That's it -- this continues on its\n");
        ui_print_bottom("   own once you approve.\n");
        ui_print_bottom("   (A = type a code in manually instead,\n");
        ui_print_bottom("   e.g. if it shows on the confirmation\n");
        ui_print_bottom("   page but doesn't continue on its own)\n\n");
    } else {
        ui_print_bottom("3. Copy the code Dropbox shows you.\n");
        ui_print_bottom("4. Press A here to type it in.\n\n");
    }
    ui_print_bottom("(B to cancel)\n\n");
    ui_print_bottom("Can't scan? Full URL:\n");
    ui_print_bottom(url);
    ui_print_bottom("\n");
    ui_flush();

    QrCode *qr = qr_prepare(url);

    char code[256] = {0};
    bool haveCode = false;
    bool cancelled = false;
    bool wantManualEntry = false;
    int frame = 0;

    RelayPollResult lastReported = RELAY_PENDING;
    while (!haveCode && !cancelled && !wantManualEntry) {
        qr_draw_frame(qr);

        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) { cancelled = true; break; }
        if (kDown & KEY_A) { wantManualEntry = true; break; }

        if (useRelay && frame > 0 && frame % 120 == 0) { // ~every 2s at 60fps
            char relayError[128] = {0};
            char diag[64] = {0};
            RelayPollResult pr = poll_relay(state, code, sizeof(code), relayError, sizeof(relayError),
                                             diag, sizeof(diag));
            if (pr == RELAY_READY) {
                haveCode = true;
            } else if (pr == RELAY_ERROR) {
                qr_free(qr);
                ui_clear();
                ui_print_error("Dropbox login failed: ");
                ui_print_error(relayError[0] ? relayError : "unknown error");
                ui_print("\n");
                ui_wait_for_a();
                return false;
            } else if (pr == RELAY_UNAVAILABLE && lastReported != RELAY_UNAVAILABLE) {
                // Report this once (not every poll) so a broken relay
                // connection is visible instead of just waiting forever
                // indistinguishably from "still pending". Printed
                // straight to the bottom screen, in the same moment as
                // the test, rather than only to the SD card debug log --
                // matching a log file to the test that produced it (SD
                // card timing, stale files, multiple consoles/cards)
                // turned out to be its own source of confusion.
                char msg[280];
                snprintf(msg, sizeof(msg),
                         "(can't reach relay: %s | certs %d/%d loaded, %d trusted "
                         "| ip=%s tls=%s %s | verify=%s | sent=%d recv=%d)\n",
                         diag[0] ? diag : "unknown",
                         http_get_loaded_cert_count(), http_get_total_cert_count(),
                         http_get_last_trusted_count(),
                         http_get_last_resolved_ip(), http_get_last_tls_version(),
                         http_get_last_tls_cipher(), http_get_last_verify_info(),
                         http_get_last_request_bytes_sent(), http_get_last_response_bytes_received());
                ui_print_bottom(msg);
                snprintf(msg, sizeof(msg), "(verify flags: raw=0x%08lX after_mask=0x%08lX)\n",
                         (unsigned long)http_get_last_verify_flags_raw(),
                         (unsigned long)http_get_last_verify_flags_after_mask());
                ui_print_bottom(msg);
            } else if (pr == RELAY_PENDING && lastReported == RELAY_UNAVAILABLE) {
                ui_print_bottom("(reached the relay again)\n");
            }
            lastReported = pr;
        }
        frame++;
        gspWaitForVBlank();
    }

    qr_free(qr);

    if (cancelled) return false;

    // Drew straight to the top screen's framebuffer, bypassing the
    // console entirely -- clear it properly now so the console's own
    // state (and the framebuffer) are back to normal before we print
    // anything through it again.
    ui_clear();
    ui_flush();

    if (!haveCode) {
        if (!swkbd_get_text("Paste the Dropbox authorization code", code, sizeof(code)) ||
            strlen(code) == 0) {
            return false;
        }
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

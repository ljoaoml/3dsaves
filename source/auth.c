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

#include <mbedtls/aes.h>

#define DROPBOX_AUTHORIZE_URL "https://www.dropbox.com/oauth2/authorize"
#define DROPBOX_TOKEN_URL "https://api.dropboxapi.com/oauth2/token"

// Lets the authorization code be delivered to the 3DS without typing it
// on the stylus keyboard (Dropbox codes are long): copy it (easy on a
// phone) into a plain text file with this name in the Konnect3DS folder
// on the SD card (e.g. via FTP homebrew), and it's picked up
// automatically. Deleted after reading so a stale one doesn't get
// reused by a later login attempt. Fallback for when the relay below
// isn't configured/used.
#define PASTE_CODE_FILE "sdmc:/3ds/Konnect3DS/paste_code.txt"

// The primary path: cloudflare-relay/ now does the *entire* OAuth
// exchange server-side (Cloudflare -> Dropbox is a normal, unblocked
// request; only the 3DS's own requests to the relay get rejected -- see
// the repo README) and its /callback page offers a download containing
// the final tokens already in this exact file format, so this is just
// the token file, delivered without the 3DS ever needing to reach the
// relay over the network. Same trim-and-delete-after-reading handling
// as PASTE_CODE_FILE.
#define PASTE_TOKENS_FILE "sdmc:/3ds/Konnect3DS/paste_tokens.txt"

static bool random_bytes(void *out, size_t size) {
    Result rc = psInit();
    if (R_FAILED(rc)) return false;
    rc = PS_GenerateRandomBytes(out, size);
    psExit();
    return R_SUCCEEDED(rc);
}

// Derives an AES-256 key from this specific console's own hardware device
// ID (PS_GetDeviceId() -- not verified against a real compiler, no
// devkitARM in the sandbox this was written in, but PS_GenerateRandomBytes
// right above already proves the psInit()/PS_*/psExit() pattern works in
// this exact file) plus a fixed app-specific string, via SHA-256 (already
// used for PKCE below). This doesn't defend against someone with the
// running console itself -- nothing purely in software can -- it defends
// against the much more likely case of a lost/stolen SD card being read
// on a PC or a different console: without this exact console's device ID,
// the token file decrypts to garbage instead of a usable Dropbox token.
static bool derive_token_key(u8 keyOut[32]) {
    Result rc = psInit();
    if (R_FAILED(rc)) return false;
    u32 deviceId = 0;
    rc = PS_GetDeviceId(&deviceId);
    psExit();
    if (R_FAILED(rc)) return false;

    static const char salt[] = "Konnect3DS-token-key-v1";
    u8 input[sizeof(deviceId) + sizeof(salt)];
    memcpy(input, &deviceId, sizeof(deviceId));
    memcpy(input + sizeof(deviceId), salt, sizeof(salt));
    sha256_buf(input, sizeof(input), keyOut);
    return true;
}

// AES-256-CTR: same operation for encrypt and decrypt (XOR against a
// keystream), so this one function serves both directions -- no separate
// decrypt key schedule needed, unlike CBC/ECB. `iv` is mutated in place
// as mbedTLS advances the counter through it; callers that still need the
// original starting IV afterward (auth_save_tokens(), to write it to the
// file) must pass a throwaway copy, not the one they still need.
static bool aes_ctr_transform(const u8 key[32], u8 iv[16], const u8 *in, u8 *out, size_t len) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, 256);
    if (ret == 0) {
        u8 streamBlock[16] = {0};
        size_t ncOff = 0;
        ret = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, iv, streamBlock, in, out);
    }
    mbedtls_aes_free(&ctx);
    return ret == 0;
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

// ui.c's log lines deliberately don't word-wrap (see its comment on
// why), so a raw URL printed as one line would just run off the right
// edge of the bottom screen past a certain length -- this is the one
// place in the app long enough for that to actually matter (it's a
// fallback for when scanning the QR code isn't possible, not the
// primary path, but should still be readable). Chunk length is a rough
// guess at how many characters fit at ui.c's TEXT_SCALE on the 320px
// bottom screen, not a measured value -- tune once this is visible on
// real hardware.
#define URL_WRAP_CHARS 42

static void print_bottom_wrapped(const char *text) {
    size_t len = strlen(text);
    for (size_t pos = 0; pos < len; pos += URL_WRAP_CHARS) {
        char chunk[URL_WRAP_CHARS + 1];
        size_t n = len - pos < URL_WRAP_CHARS ? len - pos : URL_WRAP_CHARS;
        memcpy(chunk, text + pos, n);
        chunk[n] = '\0';
        ui_print_bottom(chunk);
        ui_print_bottom("\n");
    }
}

// See PASTE_CODE_FILE's comment. Trims surrounding whitespace/newlines
// (a phone's share/export step often adds a trailing newline).
static bool check_pasted_code_file(char *codeOut, size_t codeOutSize) {
    FILE *f = fopen(PASTE_CODE_FILE, "rb");
    if (!f) return false;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(PASTE_CODE_FILE);
    buf[n] = '\0';

    size_t start = 0;
    while (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\r' || buf[start] == '\n') start++;
    size_t end = strlen(buf);
    while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' ||
                            buf[end - 1] == '\r' || buf[end - 1] == '\n')) end--;
    if (end <= start) return false;

    size_t len = end - start;
    if (len >= codeOutSize) len = codeOutSize - 1;
    memcpy(codeOut, buf + start, len);
    codeOut[len] = '\0';
    return true;
}

// Shared by auth_load_tokens() (the real token file, encrypted -- see
// derive_token_key()/aes_ctr_transform() above and their call sites
// below) and check_pasted_tokens_file() (PASTE_TOKENS_FILE, plain text --
// that one's a transient file the user drops via FTP and this app
// deletes immediately after reading, not the persistent copy that needs
// protecting). Takes an in-memory NUL-terminated buffer rather than a
// FILE* so the encrypted path can hand it a just-decrypted buffer instead
// of a real file.
static bool parse_token_file(const char *content, DropboxTokens *tokens) {
    bool haveAccess = false, haveRefresh = false;
    const char *p = content;
    while (*p) {
        const char *lineEnd = strchr(p, '\n');
        size_t lineLen = lineEnd ? (size_t)(lineEnd - p) : strlen(p);

        // Must comfortably fit "access_token=" + the token itself (see
        // DropboxTokens.access_token's comment in auth.h) + the line ending.
        char line[2200];
        size_t copyLen = lineLen < sizeof(line) - 1 ? lineLen : sizeof(line) - 1;
        memcpy(line, p, copyLen);
        line[copyLen] = '\0';
        while (copyLen > 0 && line[copyLen - 1] == '\r') line[--copyLen] = '\0';

        if (strncmp(line, "access_token=", 13) == 0) {
            strncpy(tokens->access_token, line + 13, sizeof(tokens->access_token) - 1);
            haveAccess = true;
        } else if (strncmp(line, "refresh_token=", 14) == 0) {
            strncpy(tokens->refresh_token, line + 14, sizeof(tokens->refresh_token) - 1);
            haveRefresh = true;
        } else if (strncmp(line, "expires_at=", 11) == 0) {
            tokens->expires_at_unix = strtoull(line + 11, NULL, 10);
        }

        p += lineLen;
        if (lineEnd) p++; // skip the '\n' itself
    }
    tokens->valid = haveAccess && haveRefresh;
    return tokens->valid;
}

// See PASTE_TOKENS_FILE's comment.
static bool check_pasted_tokens_file(DropboxTokens *out) {
    FILE *f = fopen(PASTE_TOKENS_FILE, "rb");
    if (!f) return false;
    char buf[4096] = {0}; // comfortably covers the worst case (see parse_token_file()'s line buffer)
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(PASTE_TOKENS_FILE);
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    return parse_token_file(buf, out);
}

// On-disk format: a 16-byte IV followed by the AES-256-CTR-encrypted
// plaintext (same "key=value\n" lines auth_save_tokens() always wrote,
// just no longer readable by just opening the file in a text editor --
// see derive_token_key()'s comment for the actual threat this addresses).
// An old plaintext file from before this existed, or a file encrypted on
// a different console, decrypts to garbage that fails every "key="
// prefix check in parse_token_file() and so is indistinguishable from
// "no token file" here -- a clean forced re-login, not a crash.
bool auth_load_tokens(DropboxTokens *tokens) {
    memset(tokens, 0, sizeof(*tokens));

    FILE *f = fopen(DROPBOX_TOKEN_FILE, "rb");
    if (!f) return false;

    u8 iv[16];
    if (fread(iv, 1, sizeof(iv), f) != sizeof(iv)) { fclose(f); return false; }

    long cipherLen = 0;
    {
        long start = ftell(f);
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        fseek(f, start, SEEK_SET);
        cipherLen = end - start;
    }
    // 8192 is a generous sanity cap, not a real limit -- the actual
    // plaintext this ever writes tops out well under 3000 bytes (see
    // auth_save_tokens()); a file this much larger is corrupt/foreign,
    // not a legitimately huge token.
    if (cipherLen <= 0 || cipherLen > 8192) { fclose(f); return false; }

    u8 *ciphertext = malloc((size_t)cipherLen);
    if (!ciphertext) { fclose(f); return false; }
    size_t readBytes = fread(ciphertext, 1, (size_t)cipherLen, f);
    fclose(f);
    if (readBytes != (size_t)cipherLen) { free(ciphertext); return false; }

    u8 key[32];
    if (!derive_token_key(key)) { free(ciphertext); return false; }

    char *plaintext = malloc((size_t)cipherLen + 1);
    if (!plaintext) { free(ciphertext); return false; }

    bool ok = aes_ctr_transform(key, iv, ciphertext, (u8 *)plaintext, (size_t)cipherLen);
    free(ciphertext);
    if (!ok) { free(plaintext); return false; }
    plaintext[cipherLen] = '\0';

    ok = parse_token_file(plaintext, tokens);
    free(plaintext);
    return ok;
}

bool auth_save_tokens(const DropboxTokens *tokens) {
    mkdir("sdmc:/3ds", 0777); // may already exist (it's the standard homebrew folder); ignore failure
    mkdir("sdmc:/3ds/Konnect3DS", 0777);

    char plaintext[3000];
    int plainLen = snprintf(plaintext, sizeof(plaintext),
                             "access_token=%s\nrefresh_token=%s\nexpires_at=%llu\n",
                             tokens->access_token, tokens->refresh_token,
                             (unsigned long long)tokens->expires_at_unix);
    if (plainLen < 0 || (size_t)plainLen >= sizeof(plaintext)) return false;

    u8 key[32];
    if (!derive_token_key(key)) return false;

    u8 iv[16];
    if (!random_bytes(iv, sizeof(iv))) return false;
    u8 ivWorking[16];
    memcpy(ivWorking, iv, sizeof(iv)); // aes_ctr_transform() mutates its iv arg; `iv` itself still needs writing below

    u8 *ciphertext = malloc((size_t)plainLen);
    if (!ciphertext) return false;
    bool ok = aes_ctr_transform(key, ivWorking, (const u8 *)plaintext, ciphertext, (size_t)plainLen);
    if (!ok) { free(ciphertext); return false; }

    FILE *f = fopen(DROPBOX_TOKEN_FILE, "wb");
    if (!f) { free(ciphertext); return false; }
    size_t wroteIv = fwrite(iv, 1, sizeof(iv), f);
    size_t wroteCipher = fwrite(ciphertext, 1, (size_t)plainLen, f);
    fclose(f);
    free(ciphertext);
    return wroteIv == sizeof(iv) && wroteCipher == (size_t)plainLen;
}

void auth_delete_tokens(void) {
    remove(DROPBOX_TOKEN_FILE);
}

static bool parse_token_response(const char *json, DropboxTokens *tokens, bool keepExistingRefresh) {
    // Extract straight into tokens->access_token/refresh_token (sized in
    // auth.h) rather than through an intermediate fixed-size local buffer
    // -- that extra hop is exactly what silently truncated a real
    // access_token to 511 bytes before, since the local buffer was
    // smaller than what Dropbox actually sends.
    if (!json_get_string(json, "access_token", tokens->access_token, sizeof(tokens->access_token))) {
        return false;
    }

    char refresh[512] = {0};
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

bool auth_run_login_flow(DropboxTokens *out, bool *wantExit) {
    memset(out, 0, sizeof(*out));

    char verifier[128], challenge[128];
    make_pkce_pair(verifier, challenge);

    // The relay (cloudflare-relay/) now does the *entire* OAuth exchange
    // server-side and hands back a ready-to-use token file -- the 3DS
    // never needs to reach it over the network at all (which doesn't
    // work anyway: the 3DS's own requests to this relay get a raw 400
    // from Cloudflare's edge on every domain tried, unrelated to
    // Dropbox, whose own servers accept this exact client fine -- see
    // the repo README). The QR code just points at the relay's /start;
    // PKCE is generated on the relay now, not here, for that path. The
    // on-device PKCE pair above is still used for the manual fallback
    // below (PASTE_CODE_FILE / typed code) when the relay isn't set up.
    bool useRelay = strcmp(RELAY_BASE_URL, "PUT_YOUR_RELAY_URL_HERE") != 0;

    char url[1024];
    if (useRelay) {
        snprintf(url, sizeof(url), "%s/start", RELAY_BASE_URL);
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
        ui_print_bottom("3. Tap the \"Download\" button on the\n");
        ui_print_bottom("   page that follows.\n");
        ui_print_bottom("4. Copy the downloaded file (not its\n");
        ui_print_bottom("   contents) to 3ds/Konnect3DS/ on\n");
        ui_print_bottom("   the SD card (e.g. via FTP) --\n");
        ui_print_bottom("   picked up automatically.\n\n");
    } else {
        ui_print_bottom("3. Copy the code Dropbox shows you.\n");
        ui_print_bottom("4a. Save it as plain text to\n");
        ui_print_bottom("    3ds/Konnect3DS/paste_code.txt\n");
        ui_print_bottom("    on the SD card (e.g. via FTP) --\n");
        ui_print_bottom("    picked up automatically, or\n");
        ui_print_bottom("4b. Press A here to type it in.\n\n");
    }
    ui_print_bottom("(B to cancel, START to exit)\n\n");
    ui_print_bottom("Can't scan? Full URL:\n");
    print_bottom_wrapped(url);
    ui_flush();

    QrCode *qr = qr_prepare(url);

    char code[256] = {0};
    bool haveCode = false;
    bool haveTokens = false;
    bool cancelled = false;
    bool wantManualEntry = false;
    int frame = 0;

    while (!haveCode && !haveTokens && !cancelled && !wantManualEntry) {
        // Draws the QR straight into this same frame's top-screen scene
        // (see qr_draw_frame()'s comment) instead of qr_display.c owning
        // its own separate present -- there's only one GPU frame per
        // vblank, and ui.c's present() already needs to draw the bottom
        // screen's instructions every frame regardless.
        ui_flush_with_top(qr_draw_frame, qr);

        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) { cancelled = true; break; }
        if (kDown & KEY_A) { wantManualEntry = true; break; }
        if (kDown & KEY_START) { cancelled = true; *wantExit = true; break; }

        if (frame > 0 && frame % 30 == 0) { // ~every 0.5s at 60fps
            if (check_pasted_tokens_file(out)) {
                haveTokens = true;
                break;
            }
            if (check_pasted_code_file(code, sizeof(code))) {
                haveCode = true;
                break;
            }
        }
        frame++;
        gspWaitForVBlank();
    }

    qr_free(qr);

    // No manual top-screen cleanup needed here anymore: unlike the old
    // raw-framebuffer QR renderer, ui_flush_with_top() draws the QR into
    // the same citro2d scene/present cycle as everything else, and
    // main.c already unconditionally clears+flushes the top screen right
    // after this function returns (every return path, including this
    // one) -- there's no leftover pixel state a screen format switch or
    // an extra double-buffer-safe clear could even leave behind.

    if (cancelled) return false;

    if (haveTokens) {
        // The relay already did the full token exchange server-side;
        // this file *is* the finished result, nothing left to do here
        // but persist it.
        auth_save_tokens(out);
        return true;
    }

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
        // main.c clears the screen and shows a generic "cancelled or
        // failed" message right after this returns -- without pausing
        // here first, this more specific reason would never actually be
        // visible to the user (nothing's drawn until a flush, and the
        // next flush is main.c's, by which point ui_clear() already
        // wiped this line).
        ui_wait_for_a();
        return false;
    }

    bool ok = false;
    if (resp.status_code == 200 && resp.body.data) {
        ok = parse_token_response((const char *)resp.body.data, out, false);
    }
    if (!ok) {
        ui_print("Login failed. Response:\n");
        if (resp.body.data) ui_print((const char *)resp.body.data);
        ui_wait_for_a(); // see the comment on the network-error path above
    }
    http_response_free(&resp);

    if (ok) auth_save_tokens(out);
    return ok;
}

bool auth_ensure_valid(DropboxTokens *tokens, bool force_refresh) {
    if (!tokens->valid || strlen(tokens->refresh_token) == 0) return false;

    if (!force_refresh) {
        u64 now = (u64)time(NULL);
        if (tokens->expires_at_unix > now) return true; // still fresh
    }

    char body[sizeof(tokens->refresh_token) + 96]; // fixed param names/client_id + the token itself
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

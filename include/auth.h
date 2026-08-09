#pragma once
#include <3ds.h>
#include <stdbool.h>

// Dropbox OAuth 2.0 "Authorization Code with PKCE, no redirect URI" flow.
// Dropbox lets an app omit redirect_uri; when it does, the authorization
// code is shown directly on the dropbox.com consent page for the user to
// copy/paste back into the app. That's the only practical flow for a
// device with no way to receive an HTTP redirect, and it needs no client
// secret embedded in the binary. See developers.dropbox.com/oauth-guide.
//
// Set your own app's client id (the "App key" from the Dropbox App
// Console, https://www.dropbox.com/developers/apps) below before building.
#ifndef DROPBOX_CLIENT_ID
#define DROPBOX_CLIENT_ID "xxd9vkjyoedihll"
#endif

// Base URL (no trailing slash) of the OAuth relay in cloudflare-relay/.
// The 3DS's own HTTPS requests to this relay always get a raw "400 Bad
// Request" straight from Cloudflare's edge -- confirmed on real hardware
// against workers.dev, a dedicated custom domain, and even an unrelated
// third-party Cloudflare-fronted site, while the exact same client works
// fine against api.dropboxapi.com. So the 3DS never talks to this relay
// over the network at all: it just shows a QR code pointing at
// <RELAY_BASE_URL>/start, and the relay does the *entire* OAuth exchange
// server-side (Cloudflare -> Dropbox is a normal, unblocked
// server-to-server request), ending with a page that offers a
// downloadable, ready-to-use token file. The user copies that file onto
// the SD card (e.g. via FTP) and it's picked up automatically -- see
// cloudflare-relay/README.md. If left as the placeholder below, login
// falls back to manual code entry (the original no-redirect-URI flow).
#ifndef RELAY_BASE_URL
#define RELAY_BASE_URL "https://konnect3ds.vgchampions.org"
#endif

#define DROPBOX_TOKEN_FILE "sdmc:/3ds/Konnect3DS/dropbox_token.txt"

typedef struct {
    char access_token[512];
    char refresh_token[256];
    u64 expires_at_unix; // 0 if unknown
    bool valid;
} DropboxTokens;

// Loads previously saved tokens from SD, if any. Returns false if none exist.
bool auth_load_tokens(DropboxTokens *tokens);

// Persists tokens to SD (overwrites any existing file).
bool auth_save_tokens(const DropboxTokens *tokens);

void auth_delete_tokens(void);

// Runs the full interactive login: prints the authorize URL to the console,
// opens the software keyboard for the user to paste the resulting code, and
// exchanges it for tokens. Blocks until done or cancelled.
// Returns true on success and fills `out`.
bool auth_run_login_flow(DropboxTokens *out);

// Refreshes the access token using the stored refresh token if it looks
// expired (or unconditionally if force_refresh is true -- worth passing
// explicitly rather than trusting expires_at_unix alone, since the 3DS's
// own clock is not reliably UTC, see main.c's startup diagnostic).
// Updates `tokens` and the on-disk copy in place.
bool auth_ensure_valid(DropboxTokens *tokens, bool force_refresh);

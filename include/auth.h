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

// Base URL (no trailing slash) of the OAuth relay in cloudflare-relay/,
// used so the 3DS doesn't need the code typed in by hand: Dropbox
// redirects the phone's browser to <RELAY_BASE_URL>/callback, and the 3DS
// polls <RELAY_BASE_URL>/poll until the code shows up. See
// cloudflare-relay/README.md for how to deploy your own. If left as the
// placeholder below, login falls back to manual code entry (the original
// no-redirect-URI flow) instead of polling.
#ifndef RELAY_BASE_URL
#define RELAY_BASE_URL "PUT_YOUR_RELAY_URL_HERE"
#endif

#define DROPBOX_TOKEN_FILE "sdmc:/Konnect3DS/dropbox_token.txt"

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
// expired (or unconditionally if force_refresh is true). Updates `tokens`
// and the on-disk copy in place.
bool auth_ensure_valid(DropboxTokens *tokens);

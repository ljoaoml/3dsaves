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
#define DROPBOX_CLIENT_ID "PUT_YOUR_DROPBOX_APP_KEY_HERE"
#endif

#define DROPBOX_TOKEN_FILE "sdmc:/3dsaves/dropbox_token.txt"

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

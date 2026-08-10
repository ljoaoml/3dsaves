#pragma once
#include "auth.h"
#include <stdbool.h>
#include <stddef.h>

// Uploads the local SD file at `localPath` to `dropboxPath` (e.g.
// "/Konnect3DS/CTR-P-AREE.zip"), overwriting any existing file there.
// On failure, writes a short human-readable reason into `errorOut`.
bool dropbox_upload_file(DropboxTokens *tokens, const char *localPath,
                          const char *dropboxPath, char *errorOut, size_t errorOutSize);

// Builds "/Konnect3DS/<sanitized name>/<sanitized name>.zip" into `out`.
// `name` is typically a game's title (from its SMDH) or a product code --
// neither is guaranteed safe as a Dropbox path component (titles can
// contain ':', '/', etc.), so this sanitizes first. See dropbox.c for the
// exact character rules.
void dropbox_build_game_path(const char *name, char *out, size_t outSize);

// Builds "/Konnect3DS/<sanitized name>" (no trailing slash) into `out` --
// the per-game folder that holds every backup uploaded for that game, one
// zip per backup instance (see dropbox_build_backup_path()).
void dropbox_build_game_folder(const char *name, char *out, size_t outSize);

// Builds "/Konnect3DS/<sanitized name>/<sanitized label>.zip" into `out`.
// Unlike dropbox_build_game_path()'s single fixed filename (always
// overwritten), `label` identifies one specific backup instance (e.g. a
// Checkpoint backup folder's own name, or a generated timestamp for a
// live-save backup) so multiple backups for the same game coexist as
// separate files instead of clobbering each other.
void dropbox_build_backup_path(const char *name, const char *label, char *out, size_t outSize);

typedef struct {
    char name[128]; // file name only (e.g. "20260810-153000.zip"), not a full path
    long size;      // bytes, -1 if Dropbox didn't report one
} DropboxBackupEntry;

// Lists the files directly inside a game's Dropbox backup folder (see
// dropbox_build_game_folder()) -- one entry per backup previously uploaded
// via dropbox_build_backup_path(). Not recursive; a folder that doesn't
// exist yet (no backups uploaded for this game so far) reports zero
// entries via *outCount, which is NOT treated as failure. Writes up to
// maxEntries into `out`. Returns false only for an actual request failure
// (network/auth/an API error other than "folder doesn't exist"), writing a
// reason into errorOut. Does not follow pagination (Dropbox's has_more) --
// the default single-request page (up to a few thousand entries) is far
// more than this app will ever create backups for one game.
bool dropbox_list_backups(DropboxTokens *tokens, const char *gameName,
                           DropboxBackupEntry *out, int maxEntries, int *outCount,
                           char *errorOut, size_t errorOutSize);

// Fetches the logged-in account's email address (POST
// /2/users/get_current_account, which takes no arguments -- Dropbox's own
// documented convention for that is a literal "null" JSON body) into
// `out`. Returns false (leaving `out` empty) on any failure; this is
// purely cosmetic (shown in the top screen's header), so callers should
// treat a failure here as "just don't show an email" rather than
// something worth surfacing to the user.
bool dropbox_get_account_email(DropboxTokens *tokens, char *out, size_t outSize);

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

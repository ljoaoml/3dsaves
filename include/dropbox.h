#pragma once
#include "auth.h"
#include <stdbool.h>

// Uploads the local SD file at `localPath` to `dropboxPath` (e.g.
// "/3dsaves/CTR-P-AREE.zip"), overwriting any existing file there.
// On failure, writes a short human-readable reason into `errorOut`.
bool dropbox_upload_file(DropboxTokens *tokens, const char *localPath,
                          const char *dropboxPath, char *errorOut, size_t errorOutSize);

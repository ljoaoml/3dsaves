#pragma once
#include <stdbool.h>

// Minimal ZIP reader matching minizip_writer.h's writer: STORE method
// only. This project only ever reads back zips it wrote itself (a
// downloaded Dropbox backup is always one of our own uploads), so there's
// no need for a general-purpose reader -- an entry using any other
// compression method is silently skipped rather than "supported".

// Extracts every entry in the zip at `zipPath` into `destDir` (an
// sdmc:/... path, created if missing, along with any subdirectories
// entries need -- entry names use '/' separators, same as
// zipw_add_file() writes them). Succeeds (and creates an empty destDir)
// for a zip with zero entries, same as saves_backup_title() succeeding
// for an empty save. Returns false only if `zipPath` can't be opened or
// its End-Of-Central-Directory record can't be found/parsed.
bool zipr_extract_all(const char *zipPath, const char *destDir);

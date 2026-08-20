#pragma once
#include <stdbool.h>
#include <stdint.h>

// Minimal ZIP reader matching minizip_writer.h's writer: STORE method
// only. This project only ever reads back zips it wrote itself (a
// downloaded Dropbox backup is always one of our own uploads), so there's
// no need for a general-purpose reader -- an entry using any other
// compression method is silently skipped rather than "supported".

// Optional progress callback for zipr_extract_all() (see below): called
// with cumulative bytes extracted so far and the total across every
// STORE entry, at least once per entry (may be called more often for a
// single large file -- see its call site). May be called many times a
// second; keep it cheap.
typedef void (*ZipProgressFn)(uint32_t bytesDone, uint32_t bytesTotal, void *userdata);

// Extracts every entry in the zip at `zipPath` into `destDir` (an
// sdmc:/... path, created if missing, along with any subdirectories
// entries need -- entry names use '/' separators, same as
// zipw_add_file() writes them). Succeeds (and creates an empty destDir)
// for a zip with zero entries, same as saves_backup_title() succeeding
// for an empty save. Returns false if `zipPath` can't be opened, its
// End-Of-Central-Directory record can't be found/parsed, or any entry
// name contains a ".." path segment or a leading '/' (rejected outright
// rather than skipped, to fail closed on an archive that isn't the kind
// this app ever writes itself -- see minizip_reader.c's
// zip_entry_name_is_safe()).
//
// `onProgress`/`userdata` are optional (NULL/NULL for none).
bool zipr_extract_all(const char *zipPath, const char *destDir,
                       ZipProgressFn onProgress, void *userdata);

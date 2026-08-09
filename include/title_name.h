#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Reads title `titleId`'s SMDH icon data (ExeFS:/icon, via
// ARCHIVE_SAVEDATA_AND_CONTENT -- same technique FBI/Checkpoint-style
// title managers use to show a friendly name instead of a product code)
// and copies its short application title, in the console's configured
// language (falling back to English), into `out` as plain ASCII --
// anything outside the printable ASCII range is replaced with '?' since
// the console's text renderer has no real Unicode font.
//
// Returns true and fills `out` on success. Returns false (leaving `out`
// as an empty string) if the title has no SMDH, isn't readable, or has
// no name in any language -- callers should fall back to showing the
// product code/title ID in that case.
bool title_get_name(u64 titleId, FS_MediaType media, char *out, size_t outSize);

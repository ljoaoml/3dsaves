#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// 48x48 RGB565 -- the SMDH's "large" icon (there's also a 24x24 "small"
// icon this project has no use for). One u16 per pixel.
#define TITLE_BIG_ICON_PIXELS (48 * 48)

// Reads title `titleId`'s SMDH (ExeFS:/icon, via ARCHIVE_SAVEDATA_AND_CONTENT
// -- same technique FBI/Checkpoint-style title managers use to show a
// friendly name/icon instead of a bare product code) and extracts both its
// name and icon in a single file read, since callers that want one
// routinely want the other too (see ui.c's icon grid).
//
// nameOut (optional, pass NULL to skip) is filled with the short
// application title, in the console's configured language (falling back
// to English), as plain ASCII -- anything outside printable ASCII becomes
// '?' since there's no Unicode font here. Left as an empty string if the
// title has no name in any language.
//
// iconOut (optional, pass NULL to skip; must hold TITLE_BIG_ICON_PIXELS
// u16s) is filled with the icon's raw pixel data, still in the SMDH's
// native tiled layout (8x8 tiles in Morton/Z-order, tiles in row-major
// order) -- that also happens to be the 3DS GPU's own tiled texture
// layout, so it can be memcpy'd straight into a C3D_Tex (see ui.c) without
// de-swizzling. Left as all-zero (opaque black) if unavailable.
//
// Returns true if a valid SMDH was read at all (even if a requested
// nameOut/iconOut field individually came up empty) -- false only if the
// title has no SMDH or it's unreadable/malformed, in which case both
// outputs are left at their empty/zeroed default.
bool title_get_info(u64 titleId, FS_MediaType media,
                     char *nameOut, size_t nameOutSize, u16 *iconOut);

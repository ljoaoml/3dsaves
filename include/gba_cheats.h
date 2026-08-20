#pragma once
#include <stdbool.h>
#include <stddef.h>

// Manages mGBA-format ".cheats" sidecar files for GBA ROMs. mGBA's 3DS build
// has no on-device UI to type in cheat codes -- cheats can only be added on a
// PC and copied over as a "<romname>.cheats" file next to the ROM, which
// mGBA auto-loads by filename match. This lets the user add a cheat directly
// on the console instead.
//
// V1 scope: each named cheat holds exactly one code line (no multi-line
// codes, no edit/delete/enable-toggle of existing entries) -- enough to cover
// the common single-line GameShark/CodeBreaker/Action Replay codes players
// copy from cheat databases.

// Computes the sidecar cheats path for a ROM, e.g. "sdmc:/roms/game.gba" ->
// "sdmc:/roms/game.cheats" (mGBA matches by replacing the ROM's extension).
void gba_cheats_path_for_rom(const char *romPath, char *out, size_t outSize);

typedef struct {
    char name[64];
} GbaCheatEntry;

// Lists the "# <name>" cheat headers already present in `cheatsPath` (file
// may not exist yet -- outCount is set to 0 in that case).
void gba_cheats_list(const char *cheatsPath, GbaCheatEntry *out, int maxEntries, int *outCount);

// Appends one new cheat (name + a single raw code line) to `cheatsPath`,
// creating the file if needed. `codeLine` is written as-is and relies on
// mGBA's own autodetection (CodeBreaker vs GameShark/Action Replay) to parse
// it back, matching how mCheatSaveFile()/GBACheatAddLine() read the format.
bool gba_cheats_add(const char *cheatsPath, const char *name, const char *codeLine);

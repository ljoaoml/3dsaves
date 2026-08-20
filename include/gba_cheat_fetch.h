#pragma once
#include <stdbool.h>
#include <stddef.h>

// Automatic GBA cheat lookup: reads a .gba ROM's own header title, matches
// it against a bundled index of libretro-database's community cheat files
// (github.com/libretro/libretro-database, the same database RetroArch
// itself uses), and downloads + converts a match straight into the exact
// ".cheats" sidecar format mGBA's own cheat loader reads (see gba_cheats.h).
// This is the GBA-side equivalent of Checkpoint's Sharkive: automatic, no
// typing a code in by hand.
//
// V1 scope: only cheats stored as a clean sequence of 8-hex-digit-address +
// 4-hex-digit-value pairs are converted (the plain CodeBreaker shape, which
// covers the large majority of the database) -- anything else, e.g. genuine
// encrypted GameShark data, is skipped rather than guessed at (see
// gba_cheat_fetch.c's convert_code_to_lines() for why).

#define GBA_CHEAT_MAX_CANDIDATES 16
#define GBA_CHEAT_FILENAME_MAX 160

// Reads the 12-byte internal title from a .gba ROM's header (offset 0xA0,
// the public GBA cartridge header layout) into `out` (must be at least 13
// bytes), trimmed of trailing padding spaces. Returns false if the file
// can't be read.
bool gba_rom_read_title(const char *romPath, char *out, size_t outSize);

// Searches the bundled cheat-file index (romfs:/gba_cheats_index.txt) for
// filenames whose own title portion (everything before the first '(')
// starts with `romTitle`, once both sides are normalized (uppercased, '-'
// treated as a space, whitespace collapsed). This works because a GBA
// header title *is* a truncated prefix of the real game title, so a clean
// prefix match reliably identifies the right game without a separate
// ID-to-name lookup table. Writes up to `maxCandidates` matching filenames
// into `candidatesOut` (each buffer at least GBA_CHEAT_FILENAME_MAX bytes)
// and returns how many were found (0 if `romTitle` is too short to match
// reliably).
int gba_cheat_search(const char *romTitle, char candidatesOut[][GBA_CHEAT_FILENAME_MAX],
                      int maxCandidates);

// Downloads `filename` from the libretro-database GBA cheat folder, parses
// it (RetroArch's simple "cheatN_desc"/"cheatN_code" format), converts
// every cheat whose code fits the supported shape, and appends any not
// already present (by name) in `cheatsPath` via gba_cheats_add_raw().
// Reports how many cheats were newly written vs. found-but-unsupported via
// the out-params (either may be NULL; already-present cheats count as
// neither). Returns false only on an outright failure (network, file
// write) -- finding zero *new* cheats because everything was either
// unsupported or already present is still a `true` return.
bool gba_cheat_fetch_and_write(const char *filename, const char *cheatsPath,
                                int *outWritten, int *outSkipped,
                                char *errorOut, size_t errorOutSize);

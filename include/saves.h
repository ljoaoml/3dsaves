#pragma once
#include "title_name.h"
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u64 titleId;
    FS_MediaType mediaType;
    char productCode[16]; // e.g. "CTR-P-AREE"
    char name[64];        // friendly name from the title's SMDH, empty if unavailable
    // Raw 48x48 RGB565 icon pixels, still SMDH/GPU-tiled (see
    // title_get_info()) -- all-zero (opaque black) if unavailable. ui.c's
    // icon grid builds a GPU texture from this once per title list
    // refresh rather than re-reading the SMDH every frame.
    u16 icon[TITLE_BIG_ICON_PIXELS];
    // Whether this title's EXTDATA archive (separate from its SAVEDATA --
    // some games, notably several Pokémon titles, keep extra data like the
    // Pokédex/photos there instead of in the main save) opens successfully.
    // Every title in this list already has an accessible SAVEDATA archive
    // (see saves_list_titles()); extdata is an optional *additional* thing
    // to back up, checked with saves_backup_title_extdata().
    bool extdataAccessible;
    // True if this title's save doesn't live in the normal SAVEDATA
    // archive at all -- some GBA Virtual Console injects (forwarder CIAs
    // made with tools like Brewtendo, or converted from a .3dsx/CIA) only
    // expose their save through a separate raw archive reached via the
    // PxiFS0 service (see saves.c's "GBA Virtual Console injects"
    // section). saves_backup_title() checks this to pick the right one of
    // two completely different backup paths.
    bool isRawGbaSave;
} InstalledTitle;

// Optional progress callback for saves_list_titles()/saves_list_all_titles()
// (see below): called after each candidate title is probed, with `current`
// counting up to `total` -- the raw AM title count for whichever media
// type (SD or the game card) is currently being scanned, so this resets
// once partway through (SD's own total, then the card's, which is always
// 0 or 1 and not worth a separate progress line -- see saves.c). Same
// (u32, u32, void*) shape as HttpProgressFn/ZipProgressFn, so main.c's
// existing report_progress() helper works here unchanged.
typedef void (*TitleScanProgressFn)(u32 current, u32 total, void *userdata);

// Lists installed titles (SD + inserted cartridge) that are both regular
// applications (0x00040000 category, minus a short list of known non-game
// system titles -- Instruction Manual, Internet Browser, title updates,
// DSi data archives) AND have an accessible save -- the same two-part
// filter Checkpoint uses (TitleQuirks::isSystemExcluded() +
// SaveDataSource::accessible()), category alone isn't a reliable "this is
// a game" signal since plenty of non-game system titles share it too. A
// title whose SAVEDATA archive doesn't open is still included if it turns
// out to be a GBA Virtual Console inject with a save reachable the other
// way (see InstalledTitle.isRawGbaSave) -- so this is really "accessible
// via *some* save path", not strictly SAVEDATA. Result is sorted by
// display name (title name, falling back to product code) for a
// predictable on-screen order. Caller must free() the returned array.
// `onProgress`/`userdata` are optional (NULL/NULL for none).
Result saves_list_titles(InstalledTitle **out, u32 *count,
                          TitleScanProgressFn onProgress, void *userdata);

// Same as saves_list_titles(), but with neither of its two exclusions
// applied: every application-category (0x00040000) title is included,
// whether or not it's on the system-title exclusion list and whether or
// not any save probe can reach it. A manual last-resort fallback ("Other
// apps..." in main.c) for a title saves_list_titles() still doesn't show
// for some reason its probes don't recognize -- expect this to include
// plenty of built-in apps with no real save at all (Mii Maker, Face
// Raiders, ...); attempting a backup on one of those just fails cleanly,
// same as any other title this project can't read. Also sorted by display
// name. Caller must free() the returned array.
Result saves_list_all_titles(InstalledTitle **out, u32 *count,
                              TitleScanProgressFn onProgress, void *userdata);

// Backs up `title`'s save into a new zip at `zipPath` on SD: every file in
// its SAVEDATA archive preserving the directory structure, or (when
// title->isRawGbaSave) the one bare save file pulled out of a GBA Virtual
// Console inject's raw save container. Returns 0 on success (even for an
// empty SAVEDATA save), or a nonzero Result on failure -- including, for a
// raw GBA save, the container existing but never having been saved to yet
// (the game needs to be launched and saved once first).
Result saves_backup_title(const InstalledTitle *title, const char *zipPath);

// Same as saves_backup_title(), but for `title`'s EXTDATA archive instead
// of its SAVEDATA -- only meaningful when title->extdataAccessible is
// true. Fails (nonzero Result) if it isn't, same as opening any other
// archive that doesn't exist would.
Result saves_backup_title_extdata(const InstalledTitle *title, const char *zipPath);

// Zips every file under `sourceDir` (an sdmc:/... path, recursively) into
// a new zip at `zipPath`, preserving the directory structure. Unlike
// saves_backup_title(), this walks a plain SD folder via stdio
// (opendir/readdir/fopen) rather than a title's FS_Archive -- for saves
// that already exist as loose files on the SD card (e.g. a Checkpoint
// backup), not still packed inside a title's own save archive. Returns 0
// on success, -1 if sourceDir can't be opened.
Result saves_backup_folder(const char *sourceDir, const char *zipPath);

// Identifies a physical DS/DSi cartridge in the game card slot from its
// ROM header alone -- no reads or writes of its save chip at all (see
// saves.c for why: reading a DS cart's actual save needs a much riskier,
// much larger SPI protocol implementation this project deliberately
// doesn't have). Returns false if no card is inserted or it's a normal
// 3DS card. On true, fills in `gameCode` (e.g. "IPKE") and `description`
// (its ROM's own title), both NUL-terminated, matching exactly how
// Checkpoint names this cart's own Checkpoint folder -- see
// CHECKPOINT_SAVES_DIR in checkpoint_saves.h for the "0x%05X <name>"
// convention normal titles use; a DS cart (no title id to build that
// from) uses "<gameCode> <description>" instead.
bool saves_detect_ds_card(char *gameCode, size_t gameCodeSize, char *description, size_t descSize);

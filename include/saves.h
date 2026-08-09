#pragma once
#include <3ds.h>
#include <stdbool.h>

typedef struct {
    u64 titleId;
    FS_MediaType mediaType;
    char productCode[16]; // e.g. "CTR-P-AREE"
    char name[64];        // friendly name from the title's SMDH, empty if unavailable
} InstalledTitle;

// Lists installed titles (SD + inserted cartridge) that are regular
// applications (i.e. plausibly have their own SAVEDATA archive). Caller
// must free() the returned array.
Result saves_list_titles(InstalledTitle **out, u32 *count);

// Opens `title`'s SAVEDATA archive and writes every file in it into a new
// zip at `zipPath` on SD, preserving the directory structure. Returns 0 on
// success (even for an empty save), or a libctru Result on failure.
Result saves_backup_title(const InstalledTitle *title, const char *zipPath);

// Zips every file under `sourceDir` (an sdmc:/... path, recursively) into
// a new zip at `zipPath`, preserving the directory structure. Unlike
// saves_backup_title(), this walks a plain SD folder via stdio
// (opendir/readdir/fopen) rather than a title's FS_Archive -- for saves
// that already exist as loose files on the SD card (e.g. a Checkpoint
// backup), not still packed inside a title's own save archive. Returns 0
// on success, -1 if sourceDir can't be opened.
Result saves_backup_folder(const char *sourceDir, const char *zipPath);

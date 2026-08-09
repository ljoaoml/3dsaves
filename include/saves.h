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

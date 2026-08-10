#pragma once
#include <3ds.h>
#include <stdbool.h>

// Where Checkpoint (github.com/BernardoGiordano/Checkpoint) puts its own
// save backups -- confirmed against Checkpoint's own source
// (3ds/source/paths.cpp): each game gets a folder here named
// "0x%05X <game name>" (the hex title unique ID, a space, then the
// sanitized game description). Also used by main.c as the folder picker's
// starting point for manually importing a Checkpoint backup.
#define CHECKPOINT_SAVES_DIR "sdmc:/3ds/Checkpoint/saves"

// Same convention, for EXTDATA instead of SAVEDATA (Checkpoint's
// Paths::extdataRoot() in the same file) -- a separate top-level folder,
// not a subfolder of CHECKPOINT_SAVES_DIR, even though both use the same
// "0x%05X <game name>" naming for the title underneath it.
#define CHECKPOINT_EXTDATA_DIR "sdmc:/3ds/Checkpoint/extdata"

typedef struct {
    char name[256];     // the backup instance's own folder name (Checkpoint names these, e.g. a timestamp)
    char fullPath[512]; // sdmc:/... path to that folder
} CheckpointBackup;

// Finds `titleId`'s Checkpoint folder -- matched by its "0x%05X " unique-ID
// prefix (see CHECKPOINT_SAVES_DIR above) rather than reconstructing
// Checkpoint's own description-sanitizing rules, which this project
// doesn't need to replicate exactly -- and lists its backup-instance
// subfolders, newest-first (plain descending name sort: Checkpoint's own
// default backup folder names are sortable timestamps, and title.cpp's
// own loadBackupList() sorts saves the same way).
//
// Writes up to maxBackups into `out`, actual count into *outCount. A
// title with no Checkpoint folder of its own (Checkpoint's never been
// used for it, or its folder doesn't match) is reported as zero backups,
// not a failure -- this returns false only if CHECKPOINT_SAVES_DIR itself
// can't be opened at all (e.g. Checkpoint has never been run on this SD
// card).
bool checkpoint_list_backups(u64 titleId, CheckpointBackup *out, int maxBackups, int *outCount);

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

// Same as checkpoint_list_backups(), but for a folder already known by
// path (see checkpoint_list_title_folders()) instead of resolving one
// from a titleId -- used for a Checkpoint-only title main.c's automatic
// filtering doesn't otherwise show (uninstalled, or excluded for some
// other reason), so there's no titleId on hand to look it up by. Always
// succeeds (reports zero backups rather than failing) since the folder
// is already known to exist.
bool checkpoint_list_backups_in_folder(const char *titleFolder, CheckpointBackup *out, int maxBackups, int *outCount);

typedef struct {
    char name[256];     // the folder's own name, e.g. "0x12345 Pokemon Y"
    char fullPath[512]; // sdmc:/... path to that folder
    u32 uniqueId;       // parsed from the "0x%05X " prefix, 0 if unparseable
} CheckpointTitleFolder;

// Lists every top-level folder directly under CHECKPOINT_SAVES_DIR --
// unlike checkpoint_list_backups() (which needs a specific titleId to
// match against), this lists everything Checkpoint has ever backed up,
// whether or not it matches a currently-installed title. The point is to
// surface backups for a title this app's own automatic filtering (or an
// uninstalled/removed game) would otherwise hide entirely -- main.c's
// "Checkpoint" home-screen tile browses this list directly. Sorted
// alphabetically by folder name. Writes up to maxFolders into `out`,
// actual count into *outCount. Returns false only if CHECKPOINT_SAVES_DIR
// itself can't be opened (Checkpoint's never been run on this SD card).
bool checkpoint_list_title_folders(CheckpointTitleFolder *out, int maxFolders, int *outCount);

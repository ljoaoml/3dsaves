#pragma once
#include <stddef.h>
#include <stdbool.h>

// Interactive SD-card folder navigator (bottom screen, reuses
// ui_run_menu's Up/Down/A/B). Starts at `startDir` (an sdmc:/... path)
// but isn't confined to it -- "Parent folder" navigates all the way up
// to the SD root if the user keeps picking it. "Select this folder"
// confirms whatever directory is currently being browsed and writes its
// full sdmc:/... path (no trailing slash) into `outPath`. B at any point
// cancels the whole picker and returns false.
bool sd_browse_pick_folder(const char *startDir, char *outPath, size_t outSize);

// Same navigation model, but for picking a single file: folders are pure
// navigation stops (no "select this folder" option), and only files whose
// name ends in `extension` (case-insensitive, include the dot, e.g. ".gba")
// are listed and selectable. Picking one writes its full sdmc:/... path into
// `outPath`. B at any point cancels and returns false.
bool sd_browse_pick_file(const char *startDir, const char *extension, char *outPath, size_t outSize);

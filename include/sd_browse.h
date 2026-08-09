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

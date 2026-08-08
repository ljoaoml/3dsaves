#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// Minimal streaming ZIP writer, STORE method only (no compression).
// Deliberately simple: pulling in zlib/deflate is a nice future upgrade,
// but STORE keeps this self-contained and easy to verify by hand, and
// save data is usually small enough that the size cost doesn't matter.

typedef struct ZipWriter ZipWriter;

// Creates `path` on SD and starts a new archive.
ZipWriter *zipw_open(const char *path);

// Adds one file entry. `name` is the path stored inside the zip (use '/'
// separators). `data`/`size` is the whole file content.
bool zipw_add_file(ZipWriter *zw, const char *name, const uint8_t *data, uint32_t size);

// Writes the central directory + EOCD and closes the underlying file.
bool zipw_close(ZipWriter *zw);

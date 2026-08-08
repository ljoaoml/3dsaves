#pragma once
#include <stddef.h>
#include <stdbool.h>

// Shows the 3DS software keyboard with the given hint/prompt text.
// Returns true and fills `out` if the user confirmed, false if cancelled.
bool swkbd_get_text(const char *hint, char *out, size_t out_size);

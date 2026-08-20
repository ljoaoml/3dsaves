#pragma once
#include <stddef.h>
#include <stdbool.h>

// Shows the 3DS software keyboard with the given hint/prompt text.
// Returns true and fills `out` if the user confirmed, false if cancelled.
// Whatever `out` already holds on entry (if anything, NUL-terminated) is
// used as the keyboard's starting text, so a caller wanting to let the
// user edit an existing value passes it in already there; a caller
// wanting to start blank just passes an empty buffer.
// `allowEmpty` controls whether a blank submission is accepted: false
// rejects it the same as cancelling (out untouched beyond being cleared,
// same as an actual cancel) -- for input that must have a value, like an
// OAuth code. true accepts it and returns true with out set to "" -- for
// input where "nothing" is itself a meaningful, intentional answer, like
// clearing a search filter.
bool swkbd_get_text(const char *hint, char *out, size_t out_size, bool allowEmpty);

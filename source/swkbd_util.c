#include "swkbd_util.h"
#include <3ds.h>
#include <string.h>

bool swkbd_get_text(const char *hint, char *out, size_t out_size, bool allowEmpty) {
    SwkbdState swkbd;
    // SWKBD_TYPE_NORMAL: full keyboard, needed since OAuth codes mix
    // upper/lowercase, digits, '-' and '_'. 2 buttons = Cancel/OK.
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, (int)out_size - 1);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetValidation(&swkbd, allowEmpty ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (out[0]) swkbdSetInitialText(&swkbd, out);

    SwkbdButton button = swkbdInputText(&swkbd, out, out_size);
    if (button != SWKBD_BUTTON_RIGHT) {
        out[0] = '\0';
        return false;
    }

    // Trim accidental leading/trailing whitespace from copy/paste.
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    size_t start = 0;
    while (out[start] == ' ') start++;
    if (start > 0) memmove(out, out + start, strlen(out + start) + 1);

    return allowEmpty || strlen(out) > 0;
}

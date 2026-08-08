#pragma once
#include <stdbool.h>
#include <stddef.h>

// Thin text-console UI: top screen is used as a scrolling log/status area,
// bottom screen shows the current menu. Deliberately console-based (no
// citro2d/citro3d) to keep the render path simple and low-risk since this
// project's real complexity is in save extraction + networking, not UI.

void ui_init(void);
void ui_exit(void);

// Clears the top screen log.
void ui_clear(void);
// Appends text to the top screen log (printf-free, pass a preformatted string).
void ui_print(const char *text);
void ui_printf(const char *fmt, ...);
// Flushes pending console output to the framebuffer.
void ui_flush(void);

// Presents `count` items (one per line, `get_label(i)` supplies the text)
// on the bottom screen and lets the user pick one with Up/Down + A.
// Returns the selected index, or -1 if the user pressed B to cancel.
typedef const char *(*ui_menu_label_fn)(int index, void *userdata);
int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata);

// Blocks until A or B is pressed. Returns true for A, false for B.
bool ui_confirm(const char *prompt);

// Blocks until any of KEY_A/KEY_B/KEY_START is pressed (for "press A to continue" screens).
void ui_wait_for_a(void);

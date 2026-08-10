#pragma once
#include <3ds/types.h>
#include <stdbool.h>
#include <stddef.h>

// Real GPU-drawn UI (citro2d/citro3d) instead of libctru's text console --
// colors, a real font, and actual layout instead of a fixed character
// grid. The public API is intentionally close to the old console-based
// ui.h it replaces (same screen model: top screen is a scrolling log,
// bottom screen shows either that same kind of log or a menu) so callers
// elsewhere in the app didn't need to change; only how it's drawn did.
// See source/ui.c for the rendering internals.

void ui_init(void);
void ui_exit(void);

// Clears the top screen log.
void ui_clear(void);
// Appends text to the top screen log (printf-free, pass a preformatted
// string). Embedded '\n' starts a new visual line; lines don't word-wrap
// (see ui.c's log_append() for why) -- a line longer than the screen is
// wide just runs past the edge instead.
void ui_print(const char *text);
void ui_printf(const char *fmt, ...);
// Presents whatever's been queued (both screens) as one GPU frame.
void ui_flush(void);

// Same as ui_flush(), but also runs draw_top(userdata) against the top
// screen's citro2d scene before it's presented -- for a caller (the QR
// login screen) that needs to draw custom citro2d content of its own
// there, interleaved with this module's normal per-frame draw, rather
// than owning a whole separate C3D frame (there's only one GPU frame per
// vblank; two independent begin/end pairs the same frame don't work).
typedef void (*ui_top_draw_fn)(void *userdata);
void ui_flush_with_top(ui_top_draw_fn draw_top, void *userdata);

// Presents `count` items (one per line, `get_label(i)` supplies the text)
// on the bottom screen and lets the user pick one with Up/Down + A.
// Returns the selected index, or -1 if the user pressed B to cancel.
typedef const char *(*ui_menu_label_fn)(int index, void *userdata);
int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata);

// Blocks until A or B is pressed. Returns true for A, false for B.
bool ui_confirm(const char *prompt);

// Blocks until any of KEY_A/KEY_B/KEY_START is pressed (for "press A to continue" screens).
void ui_wait_for_a(void);

// Top-screen status lines with color (green/red), for reporting an
// operation's outcome without cluttering ui_print's plain-text callers.
void ui_print_success(const char *text);
void ui_print_error(const char *text);

// Top-screen colored title + underline (e.g. for a screen's heading).
void ui_print_header(const char *title);

// Same as ui_clear/ui_print/ui_printf but for the bottom screen, for
// screens that need the top screen free for something else (e.g. the
// Dropbox login QR code, drawn via ui_flush_with_top()). Still use
// ui_flush() to actually present them -- there's no separate per-screen
// flush, both screens are part of the same GPU frame.
void ui_clear_bottom(void);
void ui_print_bottom(const char *text);
void ui_printf_bottom(const char *fmt, ...);
void ui_print_header_bottom(const char *title);

// Builds (and replaces any previous set of) the title icon "shelf" drawn
// across the top of the top screen in the normal log view -- get_pixels(i,
// userdata) must return either NULL ("no icon", drawn as a plain
// placeholder tile) or a pointer to TITLE_BIG_ICON_PIXELS (see
// title_name.h) u16s of raw RGB565 icon data; the pixel data is copied
// into GPU textures immediately and not referenced afterward, so a
// temporary buffer is fine. Call this again (main.c does, after every
// title list refresh) to replace the whole shelf; pass count 0 to clear
// it. ui.c intentionally doesn't know about InstalledTitle/saves.h --
// this callback indirection keeps it a generic rendering module.
typedef const u16 *(*ui_icon_pixels_fn)(int index, void *userdata);
void ui_set_home_icons(int count, ui_icon_pixels_fn get_pixels, void *userdata);

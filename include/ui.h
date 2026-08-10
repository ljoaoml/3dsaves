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

// Like ui_wait_for_a(), but auto-continues after `frames` vblanks (~1/60s
// each) if nothing is pressed first -- for informational screens (e.g.
// startup diagnostics) that shouldn't block progress on a confirmation
// nobody needs to give.
void ui_wait_briefly(int frames);

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

// Builds (and replaces any previous set of) the title icon textures used
// by ui_run_icon_grid() -- get_pixels(i, userdata) must return either NULL
// ("no icon", drawn as a plain placeholder tile) or a pointer to
// TITLE_BIG_ICON_PIXELS (see title_name.h) u16s of raw RGB565 icon data;
// the pixel data is copied into GPU textures immediately and not
// referenced afterward, so a temporary buffer is fine. Call this again
// (main.c does, after every title list refresh) to replace the whole set;
// pass count 0 to clear it. ui.c intentionally doesn't know about
// InstalledTitle/saves.h -- this callback indirection keeps it a generic
// rendering module.
typedef const u16 *(*ui_icon_pixels_fn)(int index, void *userdata);
void ui_set_home_icons(int count, ui_icon_pixels_fn get_pixels, void *userdata);

// Sets (copies in) the account email shown at the top-right of the icon
// grid's header bar. NULL or "" clears it (shown logged-out/unknown).
void ui_set_account_email(const char *email);

// Privacy toggle for the header's email display (main.c's account menu
// offers this as "hide/show email") -- purely cosmetic, doesn't affect
// what ui_set_account_email() has stored, so toggling back shows the real
// address again immediately.
void ui_toggle_email_visibility(void);
bool ui_is_email_hidden(void);

// Interactive icon grid: the top screen's main "home" page once logged
// in. Tile 0 is always a reserved "import from folder" tile (drawn from
// the loaded folder icon texture); tiles 1..N are whatever
// ui_set_home_icons() last built, in the same order. get_label(i,
// userdata) (optional; same contract as ui_run_menu()'s, but index 0
// means the folder tile) supplies the caption shown under the grid for
// whichever tile is currently highlighted. The bottom screen shows a
// fixed set of control hints (footer-anchored) for the whole time this
// runs -- there's nothing else for it to show here.
//
// Left/Right move the highlight and wrap around the whole grid; Up/Down
// move a full row, and L/R move a full page (visible rows' worth) --
// both clamp at the top/bottom instead of wrapping (see ui.c for why). A
// confirms, returning the highlighted tile's index (0 = folder/import,
// 1..N = title index N-1). B returns UI_GRID_CANCEL, START returns
// UI_GRID_EXIT, X or SELECT returns UI_GRID_ACCOUNT (X is the primary
// account-menu button -- there's a small person icon in the header as a
// visual reminder, but it isn't independently selectable, just a hint
// that X does something there; SELECT still works too), Y returns
// UI_GRID_BACKUP_ALL (back up every listed title's live save in one go)
// -- main.c tells all of these apart from a real tile pick since none of
// them are valid indices.
#define UI_GRID_CANCEL     (-1)
#define UI_GRID_EXIT       (-2)
#define UI_GRID_ACCOUNT    (-3)
#define UI_GRID_BACKUP_ALL (-4)
int ui_run_icon_grid(ui_menu_label_fn get_label, void *userdata);

// Big centered "Log in to Dropbox" prompt, shown before the icon grid the
// first time the app isn't logged in yet. Blocks until A (returns true,
// caller should then run the actual login flow) or START (returns false,
// meaning "give up" -- caller should exit the app rather than loop this
// screen forever with nothing else reachable from it).
bool ui_run_login_gate(void);

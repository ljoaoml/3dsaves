#include "ui.h"
#include "sound.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Real GPU-drawn UI via citro2d, replacing the old libctru text console.
// See ui.h for the public contract; the big structural rule this file
// follows throughout: EVERY present (ui_flush()/ui_flush_with_top())
// fully redraws BOTH screens from the current logical state
// (s_topLines/s_bottomLines/s_bottomMode), never just the screen that
// "changed". Screens are double-buffered, so only touching one screen on
// some frames and not others is exactly the bug this project already
// hit once with the old raw-framebuffer QR code (see the git history
// around auth.c's post-login-screen cleanup) -- half of one frame and
// half of the previous one, visible at once. Redrawing everything, every
// time, from a single source of truth sidesteps that whole bug class
// instead of needing to carefully track which buffer needs what.

#define TOP_W 400.0f
#define TOP_H 240.0f
#define BOTTOM_W 320.0f
#define BOTTOM_H 240.0f

#define TEXT_SCALE 0.5f
#define HEADER_SCALE 0.6f
// Approximate row pitch at TEXT_SCALE -- not measured per-line (see
// log_append()'s comment on why), so this is a first-pass estimate to
// tune once this is actually visible on real hardware.
#define LINE_HEIGHT 15.0f
#define HEADER_HEIGHT 19.0f
#define MARGIN_X 8.0f
#define MARGIN_Y 6.0f
#define VISIBLE_LOG_LINES 15 // (240 - 2*MARGIN_Y) / LINE_HEIGHT, floored

// Top-screen header bar (see draw_top_header()): "KONNECT3DS" at the left,
// the logged-in Dropbox account email at the right.
#define HEADER_BAR_HEIGHT 30.0f
#define HEADER_TEXT_SCALE 0.62f

// Icon grid (see ui_run_icon_grid()): tiles are drawn at a fixed size
// regardless of the SMDH icon's native 48x48 -- C2D_DrawImageAt's scale
// factors handle that. This is the top screen's actual page content while
// the grid is active, not a decorative header strip -- it starts right
// below the header bar and a caption line for the highlighted item's name
// is reserved below the tiles.
// 48px = the SMDH icon's own native size -- drawing at exactly that
// (rather than upscaling) avoids the blocky/stretched look nearest-
// neighbor upscaling gave it; C3D_TexSetFilter(GPU_LINEAR) below still
// smooths any remaining sub-pixel scaling.
#define ICON_TILE_SIZE 48.0f
#define ICON_TILE_GAP 14.0f
#define ICON_GRID_Y (HEADER_BAR_HEIGHT + 10.0f)
#define ICON_CAPTION_HEIGHT 22.0f

// How many drawn frames the grid's fade-in overlay (see draw_icon_grid())
// takes to fully clear -- 12 frames is ~200ms at 60fps, long enough to
// read as a deliberate transition without feeling like a delay.
#define GRID_FADE_IN_FRAMES 12

// Cleared once per present() (confirmed against devkitPro's own
// system-font example: C2D_TextBufClear() is called once per frame, not
// once per C2D_TextParse() -- multiple texts safely share one buffer
// within the same frame as long as it isn't cleared until the next
// frame). Sized for both screens' worth of lines at once, generously
// over typical usage (most lines are short menu items/status text, not
// the full 256-char worst case) rather than tuned tight.
#define TEXTBUF_GLYPHS 8192

static C3D_RenderTarget *s_top;
static C3D_RenderTarget *s_bottom;
static C2D_TextBuf s_textBuf;

// Background pattern + folder icon (see gfx/*.t3s), loaded from RomFS at
// startup. Both are optional -- a texture that fails to load (e.g. an old
// .3dsx built before the gfx/ pipeline existed, or a stripped-down RomFS)
// just leaves the plain COLOR_BG fill visible instead of crashing.
static C2D_SpriteSheet s_bgSheet;
static C2D_Image s_bgImage;
static bool s_bgLoaded;
static C2D_SpriteSheet s_folderIconSheet;
static C2D_Image s_folderIcon;
static bool s_folderIconLoaded;

// Title icon textures (see ui_set_home_icons()), drawn by the icon grid
// (ui_run_icon_grid()/draw_icon_grid()). Every title icon is built the
// same way -- 48x48 source data copied top-left-anchored into a 64x64
// GPU_RGB565 texture -- so they all share one subtexture describing that
// same 48x48-within-64x64 region; only the underlying C3D_Tex differs
// per title. `valid[i]` false means index i's C3D_Tex was never
// successfully initialized (e.g. get_pixels() returned NULL for it) and
// must not be drawn/deleted.
static const Tex3DS_SubTexture s_titleIconSubtex = {
    48, 48, 0.0f, 1.0f, 48.0f / 64.0f, 1.0f - 48.0f / 64.0f
};
static C3D_Tex *s_titleIconTex;
static bool *s_titleIconValid;
static int s_titleIconCount;
// Per-title "has this been backed up before" flag (see
// ui_set_home_backup_marks()), drawn as a small badge over the tile in
// draw_icon_grid() -- a separate array/setter from the icon textures
// above so main.c can refresh just this (cheap: no re-reading any SMDH,
// no texture rebuild) whenever a backup completes, without redoing the
// icon textures too.
static bool *s_titleBackedUp;
static int s_titleBackedUpCount;

static u32 COLOR_BG;
static u32 COLOR_TEXT;
static u32 COLOR_MUTED;
static u32 COLOR_ACCENT;
static u32 COLOR_SUCCESS;
static u32 COLOR_DANGER;
static u32 COLOR_SELECT_BG;
static u32 COLOR_SELECT_TEXT;

// Account email shown at the top-right of the header bar (see
// draw_top_header()) -- empty when logged out. Copied in, not just
// pointed at, since callers (main.c) may not keep their own buffer alive.
static char s_accountEmail[128];

// What the top screen shows in the normal ui_flush()/ui_run_menu() path
// (i.e. when ui_flush_with_top()'s custom callback isn't in play) --
// TOP_GRID is ui_run_icon_grid()'s own screen; everything else
// (ui_print()'s scrolling log, headers, error/status messages) is
// TOP_LOG, same as before this existed.
typedef enum { TOP_LOG, TOP_GRID } TopMode;
static TopMode s_topMode = TOP_LOG;

typedef enum { LOG_TEXT, LOG_RULE } LogKind;

typedef struct {
    char text[256];
    u32 color;
    LogKind kind;
    bool bold;
} LogLine;

#define MAX_LOG_LINES 200
static LogLine s_topLines[MAX_LOG_LINES];
static int s_topCount = 0;
// True right after ui_print_progress() adds/updates the top log's last
// line -- the next ui_print_progress() call overwrites it in place
// instead of appending a new one; any other top-screen print resets this
// (see their own bodies) so a normal log line is never overwritten by a
// later progress update.
static bool s_topLastLineIsProgress = false;
static LogLine s_bottomLines[MAX_LOG_LINES];
static int s_bottomCount = 0;

// What the bottom screen currently shows instead of its log -- set by
// ui_run_menu()/ui_confirm() for the duration of their own input loop,
// reset to BOTTOM_LOG before they return. The top screen is unaffected
// either way; only ui_flush_with_top()'s custom callback replaces that.
typedef enum { BOTTOM_LOG, BOTTOM_MENU, BOTTOM_CONFIRM } BottomMode;
static BottomMode s_bottomMode = BOTTOM_LOG;

static struct {
    const char *title;
    int count;
    ui_menu_label_fn getLabel;
    void *userdata;
    int selected;
    int topRow;
    bool allowAll; // see ui_run_menu_with_all()
} s_menu;

static const char *s_confirmPrompt;

// ui_run_icon_grid()'s navigation state. Tile 0 is always the reserved
// "import from folder" tile (drawn from the folder icon texture, not
// title-icon data); tiles 1..titleCount are whatever ui_set_home_icons()
// last built. getLabel (optional) supplies the caption shown under the
// grid for the highlighted tile -- index 0 for the folder tile, same
// indices as get_pixels() otherwise.
static struct {
    int count; // 1 + title count
    int cols;
    int selected;
    int scrollRow;      // target row, from the clamping logic in ui_run_icon_grid()
    float scrollOffset;  // visual row position -- eases toward scrollRow each drawn frame (draw_icon_grid())
    float pressAnim;     // 0..1 "just pressed A" flash on the selected tile, see draw_icon_grid()
    int fadeFrame;        // counts up each drawn frame from the start of ui_run_icon_grid(); drives the fade-in overlay
    ui_menu_label_fn getLabel;
    void *userdata;
} s_grid;

static void free_home_icons(void) {
    for (int i = 0; i < s_titleIconCount; i++) {
        if (s_titleIconValid[i]) C3D_TexDelete(&s_titleIconTex[i]);
    }
    free(s_titleIconTex);
    free(s_titleIconValid);
    s_titleIconTex = NULL;
    s_titleIconValid = NULL;
    s_titleIconCount = 0;
}

void ui_set_home_icons(int count, ui_icon_pixels_fn get_pixels, void *userdata) {
    free_home_icons();
    if (count <= 0) return;

    s_titleIconTex = calloc((size_t)count, sizeof(C3D_Tex));
    s_titleIconValid = calloc((size_t)count, sizeof(bool));
    if (!s_titleIconTex || !s_titleIconValid) {
        free(s_titleIconTex);
        free(s_titleIconValid);
        s_titleIconTex = NULL;
        s_titleIconValid = NULL;
        return;
    }
    s_titleIconCount = count;

    for (int i = 0; i < count; i++) {
        const u16 *pixels = get_pixels ? get_pixels(i, userdata) : NULL;
        if (!pixels) continue;
        if (!C3D_TexInit(&s_titleIconTex[i], 64, 64, GPU_RGB565)) continue;
        // Bilinear, not the default nearest -- Checkpoint's own
        // IconStore::realize() sets this for the same reason: without it,
        // any non-1:1 scaling of the icon (even the sub-pixel kind from
        // ICON_TILE_SIZE not landing exactly on integers) looks blocky.
        C3D_TexSetFilter(&s_titleIconTex[i], GPU_LINEAR, GPU_LINEAR);

        // Same top-left-anchored 48x48-in-64x64 tile copy as
        // s_titleIconSubtex describes -- see its comment. No de-swizzling
        // needed: SMDH icon data is already laid out in the GPU's own
        // tiled format (confirmed against Checkpoint's IconStore::realize
        // and 3ds-hbmenu's menuEntryParseSmdh, both of which do the exact
        // same raw memcpy).
        u16 *dest = (u16 *)s_titleIconTex[i].data;
        const u16 *src = pixels;
        for (int row = 0; row < 48; row += 8) {
            memcpy(dest, src, 48 * 8 * sizeof(u16));
            src += 48 * 8;
            dest += 64 * 8;
        }
        s_titleIconValid[i] = true;
    }
}

// Sets which title indices (same indexing as ui_set_home_icons()) get a
// "backed up before" badge drawn over their tile -- see draw_icon_grid().
// Independent from the icon textures themselves so main.c can refresh
// just this after a backup completes, without rebuilding every texture.
void ui_set_home_backup_marks(const bool *marks, int count) {
    free(s_titleBackedUp);
    s_titleBackedUp = NULL;
    s_titleBackedUpCount = 0;
    if (count <= 0 || !marks) return;

    s_titleBackedUp = malloc(sizeof(bool) * (size_t)count);
    if (!s_titleBackedUp) return;
    memcpy(s_titleBackedUp, marks, sizeof(bool) * (size_t)count);
    s_titleBackedUpCount = count;
}

// Every piece of text in this file goes through here so "bold" means one
// thing everywhere: redrawing the same parsed text 0.5px right of itself.
// citro2d's system font has no real bold weight to switch to, so this is
// a cheap approximation (thickened strokes, not a true different glyph
// set) -- good enough at the small sizes this UI uses, and it's what the
// purple/bold palette change below actually asked for.
static void draw_text_aligned(float x, float y, float scale, u32 color, const char *text, bool bold, u32 alignFlags) {
    C2D_Text t;
    C2D_TextParse(&t, s_textBuf, text);
    C2D_TextOptimize(&t);
    u32 flags = C2D_WithColor | alignFlags;
    if (bold) C2D_DrawText(&t, flags, x + 0.5f, y, 0.0f, scale, scale, color);
    C2D_DrawText(&t, flags, x, y, 0.0f, scale, scale, color);
}

static void draw_text(float x, float y, float scale, u32 color, const char *text, bool bold) {
    draw_text_aligned(x, y, scale, color, text, bold, C2D_AlignLeft);
}

void ui_set_account_email(const char *email) {
    if (email && email[0]) snprintf(s_accountEmail, sizeof(s_accountEmail), "%s", email);
    else s_accountEmail[0] = '\0';
}

// Privacy toggle for the header's email (see show_account_menu() in
// main.c) -- purely a display concern, doesn't touch s_accountEmail
// itself, so toggling it back shows the real address again with no
// re-fetch needed. Persisted as a flag file's mere existence (no content
// to parse, just presence/absence) so the choice survives an app restart
// instead of silently resetting to "shown" every launch -- loaded once in
// ui_init(), (re)written on every toggle.
#define EMAIL_HIDDEN_FLAG_PATH "sdmc:/3ds/Konnect3DS/email_hidden.flag"
static bool s_emailHidden = false;

void ui_toggle_email_visibility(void) {
    s_emailHidden = !s_emailHidden;
    if (s_emailHidden) {
        FILE *f = fopen(EMAIL_HIDDEN_FLAG_PATH, "wb");
        if (f) fclose(f);
    } else {
        remove(EMAIL_HIDDEN_FLAG_PATH);
    }
}
bool ui_is_email_hidden(void) { return s_emailHidden; }

// No asset for this (unlike the background/folder icon) -- a plain
// circle head + trapezoid shoulders, just enough to read as "account"
// at header size. Purely a visual affordance; X (see ui_run_icon_grid())
// opens the account management menu from anywhere, not by "pressing"
// this icon (there's no touch input in this app at all).
static void draw_person_icon(float cx, float cy, float scale, u32 color) {
    C2D_DrawCircleSolid(cx, cy - 3.5f * scale, 0.0f, 2.6f * scale, color);
    C2D_DrawTriangle(cx - 4.5f * scale, cy + 5.0f * scale, color,
                      cx + 4.5f * scale, cy + 5.0f * scale, color,
                      cx - 2.6f * scale, cy - 0.5f * scale, color, 0.0f);
    C2D_DrawTriangle(cx + 4.5f * scale, cy + 5.0f * scale, color,
                      cx + 2.6f * scale, cy - 0.5f * scale, color,
                      cx - 2.6f * scale, cy - 0.5f * scale, color, 0.0f);
}

// "KONNECT3DS" at the left, the logged-in account's email at the right
// (or "****** (hidden)" if ui_toggle_email_visibility() hid it; blank if
// logged out, or before ui_set_account_email() has been told otherwise)
// next to a small person icon (X opens the account menu -- see
// ui_run_icon_grid()), a rule underneath -- the top screen's persistent
// header while the icon grid is showing.
static void draw_top_header(void) {
    draw_text(MARGIN_X, 6.0f, HEADER_TEXT_SCALE, COLOR_ACCENT, "KONNECT3DS", true);
    // The person icon (X = account menu) only makes sense once there's an
    // account linked -- s_accountEmail is empty until then, so it doubles
    // as the "are we logged in" signal here rather than adding a separate
    // bool the two call sites would have to keep in sync.
    if (s_accountEmail[0]) {
        draw_person_icon(TOP_W - MARGIN_X - 8.0f, 15.0f, 1.3f, COLOR_ACCENT);
        // Plain ASCII, not a bullet/dot glyph -- the system font this app
        // uses elsewhere has no real Unicode coverage (see
        // title_name.c's utf16_field_to_ascii()), so anything fancier
        // risks a missing-glyph box instead of an actual mask.
        const char *shown = s_emailHidden ? "****** (hidden)" : s_accountEmail;
        draw_text_aligned(TOP_W - MARGIN_X - 24.0f, 8.0f, TEXT_SCALE, COLOR_ACCENT, shown, true, C2D_AlignRight);
    }
    C2D_DrawRectSolid(MARGIN_X, HEADER_BAR_HEIGHT - 4.0f, 0.0f, TOP_W - 2 * MARGIN_X, 1.5f, COLOR_ACCENT);
}

static int grid_cols(void) {
    int cols = (int)((TOP_W - 2 * MARGIN_X + ICON_TILE_GAP) / (ICON_TILE_SIZE + ICON_TILE_GAP));
    return cols < 1 ? 1 : cols;
}

static int grid_visible_rows(void) {
    float availH = TOP_H - ICON_GRID_Y - ICON_CAPTION_HEIGHT - MARGIN_Y;
    int rows = (int)((availH + ICON_TILE_GAP) / (ICON_TILE_SIZE + ICON_TILE_GAP));
    return rows < 1 ? 1 : rows;
}

static int grid_total_rows(void) {
    return (s_grid.count + s_grid.cols - 1) / s_grid.cols;
}

// A small solid triangle pointing up or down, at the grid's right edge --
// scroll indicators for when Up/Down/L/R (see ui_run_icon_grid()) have
// more rows to reach than currently fit on screen.
static void draw_scroll_caret(float cy, bool pointsUp, u32 color) {
    float x = TOP_W - MARGIN_X - 6.0f;
    if (pointsUp) {
        C2D_DrawTriangle(x - 6.0f, cy + 4.0f, color, x + 6.0f, cy + 4.0f, color, x, cy - 4.0f, color, 0.0f);
    } else {
        C2D_DrawTriangle(x - 6.0f, cy - 4.0f, color, x + 6.0f, cy - 4.0f, color, x, cy + 4.0f, color, 0.0f);
    }
}

// A filled rounded rectangle, built from citro2d's plain primitives --
// there's no rounded-rect primitive in citro2d itself. A "plus" shape
// (one rect spanning full height but inset left/right by `radius`, one
// spanning full width but inset top/bottom) covers everywhere except the
// 4 corners, then a filled circle of that same radius at each corner
// finishes it -- the standard rect+4-circles construction for this.
// `radius` is clamped to half the smaller dimension so it can't turn a
// small/thin rect inside-out.
static void draw_rounded_rect(float x, float y, float w, float h, float radius, u32 color) {
    float maxR = (w < h ? w : h) * 0.5f;
    if (radius > maxR) radius = maxR;
    if (radius < 0.0f) radius = 0.0f;

    C2D_DrawRectSolid(x + radius, y, 0.0f, w - 2.0f * radius, h, color);
    C2D_DrawRectSolid(x, y + radius, 0.0f, w, h - 2.0f * radius, color);
    if (radius > 0.0f) {
        C2D_DrawCircleSolid(x + radius, y + radius, 0.0f, radius, color);
        C2D_DrawCircleSolid(x + w - radius, y + radius, 0.0f, radius, color);
        C2D_DrawCircleSolid(x + radius, y + h - radius, 0.0f, radius, color);
        C2D_DrawCircleSolid(x + w - radius, y + h - radius, 0.0f, radius, color);
    }
}

// The icon grid itself: tiles 0/1/2 are always the three reserved
// folder-style tiles, tiles 3..s_grid.count-1 are title icons (see
// ui_set_home_icons()) -- this is the top screen's actual page content
// while ui_run_icon_grid() is active, occupying most of the screen below
// draw_top_header(), not a decorative strip. The highlighted tile gets a
// solid frame drawn behind it (in COLOR_SELECT_BG, currently the same
// purple as everything else --
// see ui_init()) before the icon itself, and the highlighted item's own
// name is captioned below the grid via s_grid.getLabel().
static void draw_icon_grid(void) {
    int cols = s_grid.cols;
    int visibleRows = grid_visible_rows();
    float step = ICON_TILE_SIZE + ICON_TILE_GAP;

    // Eases the visual scroll position toward the target row (s_grid.scrollRow,
    // set by ui_run_icon_grid()'s clamping logic) each drawn frame -- this
    // function is now called every idle frame, not just once per input
    // decision (see ui_run_icon_grid()), so a multi-row jump (L/R paging,
    // wrap-around) glides instead of snapping straight to the target.
    s_grid.scrollOffset += ((float)s_grid.scrollRow - s_grid.scrollOffset) * 0.3f;
    if (fabsf((float)s_grid.scrollRow - s_grid.scrollOffset) < 0.02f) {
        s_grid.scrollOffset = (float)s_grid.scrollRow;
    }

    // A slow "breathing" pulse on the selected tile's highlight -- purely
    // cosmetic. Driven by a persistent counter incremented once per call
    // (one call per rendered frame while the grid is up) rather than
    // osGetTime(), so it can't ever jump if a frame takes longer than
    // usual, just steps at whatever rate frames actually render.
    static u32 s_pulseFrame = 0;
    // Wrapped well short of where a growing float's precision would
    // start eroding the 0.08f-per-frame phase step (imperceptible after
    // hours of continuous uptime otherwise, but free to just avoid) --
    // 100000 isn't an exact multiple of the ~78.5-frame period, so
    // wrapping causes one negligible phase hiccup roughly every half
    // hour rather than a real jump cut.
    s_pulseFrame = (s_pulseFrame + 1) % 100000;
    float pulse = 0.5f + 0.5f * sinf((float)s_pulseFrame * 0.08f); // 0..1, ~1.3s period at 60fps
    // s_grid.pressAnim (see ui_run_icon_grid()'s KEY_A handling) briefly
    // boosts both on top of the ambient pulse right after A is pressed,
    // for a quick "confirm" flash before the tile's actually opened.
    float highlightPad = 4.0f + pulse * 2.0f + s_grid.pressAnim * 3.0f;       // 4..6px, +0..3px on press
    float highlightAlphaF = 170.0f + pulse * 85.0f + s_grid.pressAnim * 60.0f; // 170..255, +0..60 on press
    if (highlightAlphaF > 255.0f) highlightAlphaF = 255.0f;
    u32 highlightColor = C2D_Color32(0x58, 0x54, 0xC1, (u8)highlightAlphaF); // COLOR_SELECT_BG's RGB, animated alpha

    for (int i = 0; i < s_grid.count; i++) {
        int row = i / cols;
        // Slack of 1 row on each side of the culling window (rather than
        // exactly [scrollOffset, scrollOffset+visibleRows)) so a tile
        // scrolling into or out of view still draws while the offset is
        // mid-animation, instead of popping in right at the edge.
        if ((float)row < s_grid.scrollOffset - 1.0f || (float)row > s_grid.scrollOffset + (float)visibleRows) continue;
        int col = i % cols;
        float x = MARGIN_X + col * step;
        float y = ICON_GRID_Y + ((float)row - s_grid.scrollOffset) * step;

        if (i == s_grid.selected) {
            draw_rounded_rect(x - highlightPad, y - highlightPad,
                               ICON_TILE_SIZE + highlightPad * 2.0f, ICON_TILE_SIZE + highlightPad * 2.0f,
                               8.0f, highlightColor);
        }

        if (i == 0 || i == 1 || i == 2) {
            // Tile 0 (import), tile 1 (browse Checkpoint's own saves
            // folder) and tile 2 (browse every installed app, unfiltered)
            // share the one folder-icon asset there is; the caption below
            // tells them apart, same as every other tile relies on
            // getLabel() rather than a unique icon per entry.
            if (s_folderIconLoaded) {
                C2D_DrawImageAt(s_folderIcon, x, y, 0.0f, NULL,
                                 ICON_TILE_SIZE / s_folderIcon.subtex->width,
                                 ICON_TILE_SIZE / s_folderIcon.subtex->height);
            } else {
                C2D_DrawRectSolid(x, y, 0.0f, ICON_TILE_SIZE, ICON_TILE_SIZE, COLOR_MUTED);
            }
            continue;
        }

        int titleIdx = i - 3;
        if (titleIdx < s_titleIconCount && s_titleIconValid[titleIdx]) {
            C2D_Image img = { &s_titleIconTex[titleIdx], &s_titleIconSubtex };
            C2D_DrawImageAt(img, x, y, 0.0f, NULL, ICON_TILE_SIZE / 48.0f, ICON_TILE_SIZE / 48.0f);
        } else {
            C2D_DrawRectSolid(x, y, 0.0f, ICON_TILE_SIZE, ICON_TILE_SIZE, COLOR_MUTED);
        }

        // "Already backed up before" badge: a small filled dot over the
        // tile's top-right corner, on top of whatever was just drawn (icon
        // or placeholder) -- see ui_set_home_backup_marks(). A white ring
        // behind it keeps it visible over any icon color underneath.
        if (titleIdx < s_titleBackedUpCount && s_titleBackedUp[titleIdx]) {
            float bx = x + ICON_TILE_SIZE - 3.0f;
            float by = y + 3.0f;
            C2D_DrawCircleSolid(bx, by, 0.0f, 6.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
            C2D_DrawCircleSolid(bx, by, 0.0f, 4.0f, COLOR_SUCCESS);
        }
    }

    if (s_grid.scrollRow > 0) {
        draw_scroll_caret(ICON_GRID_Y - 6.0f, true, COLOR_ACCENT);
    }
    if (s_grid.scrollRow + visibleRows < grid_total_rows()) {
        draw_scroll_caret(ICON_GRID_Y + visibleRows * step - ICON_TILE_GAP * 0.5f, false, COLOR_ACCENT);
    }

    if (s_grid.getLabel) {
        const char *caption = s_grid.getLabel(s_grid.selected, s_grid.userdata);
        if (caption) {
            float captionY = TOP_H - MARGIN_Y - ICON_CAPTION_HEIGHT + 4.0f;
            draw_text(MARGIN_X, captionY, TEXT_SCALE, COLOR_ACCENT, caption, true);
        }
    }

    // Fades the whole top screen in from the background color over the
    // first GRID_FADE_IN_FRAMES frames of each ui_run_icon_grid() call --
    // makes returning to the home screen (the single most common
    // transition in this app -- every sub-screen leads back here) read as
    // a deliberate transition instead of an instant hard cut. Drawn last
    // so it covers draw_top_header() too (called before this in
    // present()), not just the grid itself. The RGB come straight from
    // COLOR_BG (mask off its alpha byte, splice in this frame's own) so
    // this always matches the real background color, not a duplicated
    // literal that could drift out of sync with ui_init()'s palette.
    if (s_grid.fadeFrame < GRID_FADE_IN_FRAMES) {
        float t = 1.0f - (float)s_grid.fadeFrame / (float)GRID_FADE_IN_FRAMES;
        u32 fadeColor = (COLOR_BG & 0x00FFFFFFu) | ((u32)(t * 255.0f) << 24);
        C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, TOP_W, TOP_H, fadeColor);
        s_grid.fadeFrame++;
    }
}

void ui_init(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_textBuf = C2D_TextBufNew(TEXTBUF_GLYPHS);

    // Requires romfsInit() to have already run (main() calls it before
    // ui_init() for exactly this reason) -- these are RomFS paths.
    s_bgSheet = C2D_SpriteSheetLoad("romfs:/gfx/bgpattern.t3x");
    s_bgLoaded = s_bgSheet != NULL;
    if (s_bgLoaded) s_bgImage = C2D_SpriteSheetGetImage(s_bgSheet, 0);

    s_folderIconSheet = C2D_SpriteSheetLoad("romfs:/gfx/folder_icon.t3x");
    s_folderIconLoaded = s_folderIconSheet != NULL;
    if (s_folderIconLoaded) {
        s_folderIcon = C2D_SpriteSheetGetImage(s_folderIconSheet, 0);
        // Drawn well below its native resolution (grid tile size vs. the
        // source PNG) -- linear filtering avoids aliasing on that downscale.
        C3D_TexSetFilter(s_folderIcon.tex, GPU_LINEAR, GPU_LINEAR);
    }

    // Restores the "hide email" preference from a previous run (see
    // EMAIL_HIDDEN_FLAG_PATH's comment) -- safe to check before main()'s
    // own mkdir("sdmc:/3ds/Konnect3DS", ...) has run: a missing folder
    // just means fopen() returns NULL here, same as a missing file would.
    {
        FILE *f = fopen(EMAIL_HIDDEN_FLAG_PATH, "rb");
        if (f) { s_emailHidden = true; fclose(f); }
    }

    // #5854C1 -- the exact purple from the mockup/folder icon asset (its
    // source file was literally named "#5854c1.png") -- as the one accent
    // color text is drawn in throughout, per feedback that the previous
    // near-white palette was invisible against the new light background
    // pattern. COLOR_BG is just the fallback if that background texture
    // fails to load (see draw_background()), so it's a light cream in the
    // same family rather than the near-black it used to be, for the same
    // reason. Easy to retune further once seen on hardware; nothing else
    // in this file depends on the specific values.
    COLOR_BG          = C2D_Color32(0xFF, 0xF6, 0xE3, 0xFF);
    COLOR_TEXT        = C2D_Color32(0x58, 0x54, 0xC1, 0xFF);
    COLOR_MUTED       = C2D_Color32(0xA8, 0xA4, 0xE0, 0xFF);
    COLOR_ACCENT      = C2D_Color32(0x58, 0x54, 0xC1, 0xFF);
    COLOR_SUCCESS     = C2D_Color32(0x2E, 0x7D, 0x32, 0xFF);
    COLOR_DANGER      = C2D_Color32(0xC6, 0x28, 0x28, 0xFF);
    COLOR_SELECT_BG   = C2D_Color32(0x58, 0x54, 0xC1, 0xFF);
    COLOR_SELECT_TEXT = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
}

void ui_exit(void) {
    free_home_icons();
    free(s_titleBackedUp);
    s_titleBackedUp = NULL;
    s_titleBackedUpCount = 0;
    if (s_folderIconLoaded) C2D_SpriteSheetFree(s_folderIconSheet);
    if (s_bgLoaded) C2D_SpriteSheetFree(s_bgSheet);
    C2D_TextBufDelete(s_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

// Stretches the background image to exactly fill a screenW x screenH
// target -- slight aspect distortion (400x240 top vs 320x240 bottom, vs.
// the source image's own aspect ratio) is an acceptable tradeoff for
// "always fills the screen with no letterboxing" over pixel-perfect aspect.
// flipVertical mirrors it top-to-bottom (negative scaleY, anchored at the
// bottom edge instead of the top) -- the mockup's bottom-screen background
// was vertically flipped so its diagonal reads as a continuation of the
// top screen's rather than a plain repeat of it.
static void draw_background(float screenW, float screenH, bool flipVertical) {
    if (!s_bgLoaded) return;
    float scaleX = screenW / s_bgImage.subtex->width;
    float scaleY = screenH / s_bgImage.subtex->height;
    if (flipVertical) {
        C2D_DrawImageAt(s_bgImage, 0.0f, screenH, 0.0f, NULL, scaleX, -scaleY);
    } else {
        C2D_DrawImageAt(s_bgImage, 0.0f, 0.0f, 0.0f, NULL, scaleX, scaleY);
    }
}

static void log_append_line(LogLine *lines, int *count, const char *text, u32 color, LogKind kind, bool bold) {
    if (*count >= MAX_LOG_LINES) {
        memmove(lines, lines + 1, sizeof(LogLine) * (MAX_LOG_LINES - 1));
        (*count)--;
    }
    LogLine *dst = &lines[*count];
    size_t len = strlen(text);
    size_t copyLen = len < sizeof(dst->text) - 1 ? len : sizeof(dst->text) - 1;
    memcpy(dst->text, text, copyLen);
    dst->text[copyLen] = '\0';
    dst->color = color;
    dst->kind = kind;
    dst->bold = bold;
    (*count)++;
}

// Splits `text` on '\n' into separate log lines. Deliberately doesn't
// word-wrap long lines: with a proportional font, correctly reserving
// vertical space for a wrapped line needs its rendered height measured
// (C2D_TextGetDimensions) before laying out anything below it, and doing
// that against a fixed LINE_HEIGHT step risks overlapping text -- worse
// than a long line just running past the right edge, which is what
// happens here instead. Only a couple of call sites (auth.c's raw OAuth
// URL fallback) are long enough for this to show; revisit if real
// hardware testing shows it's more of a problem in practice than that.
static void log_append(LogLine *lines, int *count, const char *text, u32 color) {
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char buf[256];
        size_t copyLen = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, p, copyLen);
        buf[copyLen] = '\0';
        log_append_line(lines, count, buf, color, LOG_TEXT, false);
        if (!nl) break;
        p = nl + 1;
    }
}

static void draw_log(const LogLine *lines, int count, float screenW, float startY) {
    // VISIBLE_LOG_LINES assumes a MARGIN_Y start; when startY is pushed
    // down (top screen, header/grid present), a couple of lines' worth can
    // run past the bottom edge before scrolling catches up -- cosmetic
    // scroll-lag, not a crash, and not worth a second constant for.
    int start = count > VISIBLE_LOG_LINES ? count - VISIBLE_LOG_LINES : 0;
    float y = startY;
    for (int i = start; i < count; i++) {
        const LogLine *line = &lines[i];
        if (line->kind == LOG_RULE) {
            C2D_DrawRectSolid(MARGIN_X, y + LINE_HEIGHT * 0.5f, 0.0f, screenW - 2 * MARGIN_X, 1.5f, line->color);
            y += LINE_HEIGHT;
            continue;
        }
        if (line->text[0] == '\0') { y += LINE_HEIGHT; continue; }

        draw_text(MARGIN_X, y, TEXT_SCALE, line->color, line->text, line->bold);
        y += LINE_HEIGHT;
    }
}

static void draw_menu(void) {
    float y = MARGIN_Y;

    draw_text(MARGIN_X, y, HEADER_SCALE, COLOR_ACCENT, s_menu.title, true);
    y += HEADER_HEIGHT;
    C2D_DrawRectSolid(MARGIN_X, y, 0.0f, BOTTOM_W - 2 * MARGIN_X, 1.5f, COLOR_ACCENT);
    y += LINE_HEIGHT * 0.6f;

    const int visibleRows = 10;
    int topRow = s_menu.topRow;
    for (int i = topRow; i < s_menu.count && i < topRow + visibleRows; i++) {
        bool isSelected = (i == s_menu.selected);
        const char *label = s_menu.getLabel(i, s_menu.userdata);
        if (isSelected) {
            C2D_DrawRectSolid(MARGIN_X - 2, y - 1, 0.0f, BOTTOM_W - 2 * (MARGIN_X - 2), LINE_HEIGHT, COLOR_SELECT_BG);
        }

        draw_text(MARGIN_X, y, TEXT_SCALE, isSelected ? COLOR_SELECT_TEXT : COLOR_TEXT, label ? label : "?", false);
        y += LINE_HEIGHT;
    }

    if (s_menu.allowAll) {
        y = BOTTOM_H - MARGIN_Y - 2 * LINE_HEIGHT;
        draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "Up/Down select, A confirm", false);
        y += LINE_HEIGHT;
        draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "Y: back up all, B: cancel", false);
    } else {
        y = BOTTOM_H - MARGIN_Y - LINE_HEIGHT;
        draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "Up/Down select, A confirm, B cancel", false);
    }
}

static void draw_confirm(void) {
    float y = MARGIN_Y;

    draw_text(MARGIN_X, y, HEADER_SCALE, COLOR_ACCENT, s_confirmPrompt, true);
    y += HEADER_HEIGHT;
    C2D_DrawRectSolid(MARGIN_X, y, 0.0f, BOTTOM_W - 2 * MARGIN_X, 1.5f, COLOR_ACCENT);
    y += LINE_HEIGHT * 1.5f;

    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "A = yes    B = no", false);
}

#define GRID_FOOTER_LINES 7

// The icon grid's bottom screen: fixed control hints, anchored to the
// bottom of the screen (a footer) rather than the top -- there's nothing
// else to show there while the grid is up, so this bypasses the generic
// top-anchored scrolling log entirely instead of stretching it to fake a
// footer.
static void draw_grid_bottom_footer(void) {
    float y = BOTTOM_H - MARGIN_Y - GRID_FOOTER_LINES * LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "D-Pad: navigate", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "L / R: jump a page", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "A: open", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "X: manage account", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "Y: back up everything", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "SELECT: extra data only", false); y += LINE_HEIGHT;
    draw_text(MARGIN_X, y, TEXT_SCALE, COLOR_MUTED, "START: exit app", false);
}

static void present(ui_top_draw_fn drawTop, void *topUserdata) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    // Cleared once for the whole frame, not per line -- see
    // TEXTBUF_GLYPHS's comment. Both screens' text below shares this one
    // buffer.
    C2D_TextBufClear(s_textBuf);

    C2D_TargetClear(s_top, COLOR_BG);
    C2D_SceneBegin(s_top);
    draw_background(TOP_W, TOP_H, false);
    if (drawTop) {
        drawTop(topUserdata);
    } else if (s_topMode == TOP_GRID) {
        draw_top_header();
        draw_icon_grid();
    } else {
        draw_log(s_topLines, s_topCount, TOP_W, MARGIN_Y);
    }

    C2D_TargetClear(s_bottom, COLOR_BG);
    C2D_SceneBegin(s_bottom);
    draw_background(BOTTOM_W, BOTTOM_H, true);
    switch (s_bottomMode) {
        case BOTTOM_MENU:    draw_menu(); break;
        case BOTTOM_CONFIRM: draw_confirm(); break;
        default:
            if (s_topMode == TOP_GRID) draw_grid_bottom_footer();
            else draw_log(s_bottomLines, s_bottomCount, BOTTOM_W, MARGIN_Y);
            break;
    }

    C3D_FrameEnd(0);
}

void ui_flush(void) { present(NULL, NULL); }
void ui_flush_with_top(ui_top_draw_fn draw_top, void *userdata) { present(draw_top, userdata); }

void ui_clear(void) { s_topCount = 0; s_topLastLineIsProgress = false; }

void ui_print(const char *text) {
    s_topLastLineIsProgress = false;
    log_append(s_topLines, &s_topCount, text, COLOR_TEXT);
}

void ui_printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ui_print(buf);
}

void ui_print_success(const char *text) {
    s_topLastLineIsProgress = false;
    log_append(s_topLines, &s_topCount, text, COLOR_SUCCESS);
    sound_play_success();
}
void ui_print_error(const char *text) {
    s_topLastLineIsProgress = false;
    log_append(s_topLines, &s_topCount, text, COLOR_DANGER);
    sound_play_error();
}

// Updates (or starts) a single "live status" line on the top screen's log
// -- repeated calls overwrite the same line instead of appending a new
// one each time, for something like a percentage counter that changes
// many times a second without flooding the scrolling log. `text` must not
// contain '\n' (a single line only, unlike ui_print()). Any of
// ui_clear()/ui_print()/ui_printf()/ui_print_success()/ui_print_error()
// ends the "replaceable" streak, so normal log lines that follow a
// progress sequence still append as usual, not overwrite it.
void ui_print_progress(const char *text) {
    if (s_topLastLineIsProgress && s_topCount > 0) {
        LogLine *dst = &s_topLines[s_topCount - 1];
        size_t len = strlen(text);
        size_t copyLen = len < sizeof(dst->text) - 1 ? len : sizeof(dst->text) - 1;
        memcpy(dst->text, text, copyLen);
        dst->text[copyLen] = '\0';
    } else {
        log_append_line(s_topLines, &s_topCount, text, COLOR_TEXT, LOG_TEXT, false);
        s_topLastLineIsProgress = true;
    }
    ui_flush();
}

void ui_print_header(const char *title) {
    log_append_line(s_topLines, &s_topCount, title, COLOR_ACCENT, LOG_TEXT, true);
    log_append_line(s_topLines, &s_topCount, "", COLOR_ACCENT, LOG_RULE, false);
}

void ui_clear_bottom(void) { s_bottomCount = 0; }
void ui_print_bottom(const char *text) { log_append(s_bottomLines, &s_bottomCount, text, COLOR_TEXT); }

void ui_printf_bottom(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ui_print_bottom(buf);
}

void ui_print_header_bottom(const char *title) {
    log_append_line(s_bottomLines, &s_bottomCount, title, COLOR_ACCENT, LOG_TEXT, true);
    log_append_line(s_bottomLines, &s_bottomCount, "", COLOR_ACCENT, LOG_RULE, false);
}

static int run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata, bool allowAll) {
    s_menu.title = title;
    s_menu.count = count;
    s_menu.getLabel = get_label;
    s_menu.userdata = userdata;
    s_menu.selected = 0;
    s_menu.topRow = 0;
    s_menu.allowAll = allowAll;
    s_bottomMode = BOTTOM_MENU;

    const int visibleRows = 10;
    int result = -1;

    while (true) {
        if (s_menu.selected < s_menu.topRow) s_menu.topRow = s_menu.selected;
        if (s_menu.selected >= s_menu.topRow + visibleRows) s_menu.topRow = s_menu.selected - visibleRows + 1;

        ui_flush();

        bool decided = false;
        while (!decided) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_DOWN) { s_menu.selected = (s_menu.selected + 1) % count; decided = true; }
            else if (kDown & KEY_UP) { s_menu.selected = (s_menu.selected - 1 + count) % count; decided = true; }
            else if (kDown & KEY_A) { result = s_menu.selected; goto done; }
            else if (allowAll && (kDown & KEY_Y)) { result = UI_MENU_ALL; goto done; }
            else if (kDown & KEY_B) { result = -1; goto done; }
            else gspWaitForVBlank();
        }
    }

done:
    s_bottomMode = BOTTOM_LOG;
    return result;
}

int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata) {
    return run_menu(title, count, get_label, userdata, false);
}

int ui_run_menu_with_all(const char *title, int count, ui_menu_label_fn get_label, void *userdata) {
    return run_menu(title, count, get_label, userdata, true);
}

bool ui_confirm(const char *prompt) {
    s_confirmPrompt = prompt;
    s_bottomMode = BOTTOM_CONFIRM;
    ui_flush();

    bool result = false;
    while (true) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) { result = true; break; }
        if (kDown & KEY_B) { result = false; break; }
        gspWaitForVBlank();
    }

    s_bottomMode = BOTTOM_LOG;
    return result;
}

int ui_run_icon_grid(ui_menu_label_fn get_label, void *userdata) {
    s_grid.count = 3 + s_titleIconCount; // tile 0 = import, tile 1 = Checkpoint, tile 2 = other apps
    s_grid.cols = grid_cols();
    if (s_grid.selected >= s_grid.count) s_grid.selected = 0;
    s_grid.scrollRow = 0;
    s_grid.scrollOffset = 0.0f; // fresh session -- no scroll animation on first appearance, only for in-session moves
    s_grid.pressAnim = 0.0f;
    s_grid.fadeFrame = 0;
    s_grid.getLabel = get_label;
    s_grid.userdata = userdata;
    s_topMode = TOP_GRID;
    s_bottomMode = BOTTOM_LOG; // draw_grid_bottom_footer() takes over from here (see present())

    int visibleRows = grid_visible_rows();
    int result;

    while (true) {
        int selRow = s_grid.selected / s_grid.cols;
        if (selRow < s_grid.scrollRow) s_grid.scrollRow = selRow;
        if (selRow >= s_grid.scrollRow + visibleRows) s_grid.scrollRow = selRow - visibleRows + 1;

        // Redrawn every idle frame now (not just once per input decision,
        // which the loop used to do) so the pulse highlight, scroll
        // easing and fade-in (see draw_icon_grid()) actually animate
        // continuously while sitting idle, instead of only advancing
        // once each time a key happens to get pressed -- same
        // draw-then-wait-for-vblank shape ui_run_login_gate() already uses.
        bool decided = false;
        while (!decided) {
            ui_flush();
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_RIGHT) {
                s_grid.selected = (s_grid.selected + 1) % s_grid.count;
                decided = true;
            } else if (kDown & KEY_LEFT) {
                s_grid.selected = (s_grid.selected - 1 + s_grid.count) % s_grid.count;
                decided = true;
            } else if (kDown & KEY_DOWN) {
                // Clamp rather than wrap: with a possibly-ragged last row,
                // wrapping cleanly needs more bookkeeping than a homebrew
                // grid picker is worth -- "nothing happens at the edge" is
                // a safe, easy-to-verify fallback that also just cannot
                // land on an out-of-range index.
                int next = s_grid.selected + s_grid.cols;
                if (next < s_grid.count) { s_grid.selected = next; decided = true; }
            } else if (kDown & KEY_UP) {
                int next = s_grid.selected - s_grid.cols;
                if (next >= 0) { s_grid.selected = next; decided = true; }
            } else if (kDown & KEY_R) {
                // Fast-scroll a full page (Checkpoint's own convention:
                // DPAD moves one at a time, L/R jump further) -- clamped
                // to the last tile rather than wrapping, same reasoning
                // as Up/Down above.
                int next = s_grid.selected + visibleRows * s_grid.cols;
                s_grid.selected = next < s_grid.count ? next : s_grid.count - 1;
                decided = true;
            } else if (kDown & KEY_L) {
                int next = s_grid.selected - visibleRows * s_grid.cols;
                s_grid.selected = next >= 0 ? next : 0;
                decided = true;
            } else if (kDown & KEY_A) {
                // A brief "confirm" flash on the selected tile (see
                // draw_icon_grid()'s use of s_grid.pressAnim) before
                // actually committing the pick -- still drawn through
                // ui_flush() each of these frames, so the background/
                // header/footer stay exactly as they were.
                for (int f = 0; f < 6; f++) {
                    s_grid.pressAnim = (float)(f + 1) / 6.0f;
                    ui_flush();
                    gspWaitForVBlank();
                }
                result = s_grid.selected;
                goto done;
            } else if (kDown & KEY_B) { result = UI_GRID_CANCEL; goto done; }
            else if (kDown & KEY_START) { result = UI_GRID_EXIT; goto done; }
            else if (kDown & KEY_X) { result = UI_GRID_ACCOUNT; goto done; }
            else if (kDown & KEY_Y) { result = UI_GRID_BACKUP_ALL; goto done; }
            else if (kDown & KEY_SELECT) { result = UI_GRID_TOGGLE_EXTDATA; goto done; }
            else gspWaitForVBlank();
        }
    }

done:
    s_topMode = TOP_LOG;
    return result;
}

// The very first thing shown on launch (see main()) -- big centered app
// name, held for `frames` vblanks (skippable with A/B/START, same
// contract as ui_wait_briefly()) and self-fading to the background color
// over its last FADE_OUT_FRAMES frames so the handoff into the login
// gate/icon grid right after reads as a transition rather than a hard
// cut. Doesn't go through present()/s_topMode+s_bottomMode, same reason
// as ui_run_login_gate() right below: there's no log/menu/grid state to
// show yet, so this just draws both screens directly in its own loop.
void ui_show_splash(int frames) {
    const int fadeOutFrames = 15;
    for (int i = 0; i < frames; i++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(s_textBuf);

        C2D_TargetClear(s_top, COLOR_BG);
        C2D_SceneBegin(s_top);
        draw_background(TOP_W, TOP_H, false);
        draw_text_aligned(TOP_W / 2.0f, TOP_H / 2.0f - 16.0f, 1.1f, COLOR_ACCENT,
                           "KONNECT3DS", true, C2D_AlignCenter);
        draw_text_aligned(TOP_W / 2.0f, TOP_H / 2.0f + 20.0f, TEXT_SCALE, COLOR_MUTED,
                           "Game saves, backed up to Dropbox", false, C2D_AlignCenter);
        if (i >= frames - fadeOutFrames) {
            float t = (float)(i - (frames - fadeOutFrames)) / (float)fadeOutFrames;
            if (t < 0.0f) t = 0.0f; // frames < fadeOutFrames: fading from the very first drawn frame
            if (t > 1.0f) t = 1.0f;
            u32 fadeColor = (COLOR_BG & 0x00FFFFFFu) | ((u32)(t * 255.0f) << 24);
            C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, TOP_W, TOP_H, fadeColor);
        }

        C2D_TargetClear(s_bottom, COLOR_BG);
        C2D_SceneBegin(s_bottom);
        draw_background(BOTTOM_W, BOTTOM_H, true);

        C3D_FrameEnd(0);

        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & (KEY_A | KEY_B | KEY_START)) break;
        gspWaitForVBlank();
    }
}

// Doesn't go through present()/s_topMode+s_bottomMode: this is the very
// first screen (before there's any title list or grid state to show), so
// it just draws both screens directly in its own frame loop -- still
// redrawing both together every frame, same invariant as present(), just
// inlined instead of routed through the shared log/menu/grid dispatch.
bool ui_run_login_gate(void) {
    while (true) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(s_textBuf);

        C2D_TargetClear(s_top, COLOR_BG);
        C2D_SceneBegin(s_top);
        draw_background(TOP_W, TOP_H, false);
        draw_top_header();
        draw_text_aligned(TOP_W / 2.0f, TOP_H / 2.0f - 30.0f, 0.9f, COLOR_ACCENT,
                           "Log in to Dropbox", true, C2D_AlignCenter);
        draw_text_aligned(TOP_W / 2.0f, TOP_H / 2.0f + 6.0f, TEXT_SCALE, COLOR_MUTED,
                           "Press A to continue", false, C2D_AlignCenter);

        C2D_TargetClear(s_bottom, COLOR_BG);
        C2D_SceneBegin(s_bottom);
        draw_background(BOTTOM_W, BOTTOM_H, true);
        draw_text_aligned(BOTTOM_W / 2.0f, BOTTOM_H / 2.0f, TEXT_SCALE, COLOR_MUTED,
                           "A = login    START = exit", false, C2D_AlignCenter);

        C3D_FrameEnd(0);

        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) return true;
        if (kDown & KEY_START) return false;
        gspWaitForVBlank();
    }
}

void ui_wait_briefly(int frames) {
    ui_flush();
    for (int i = 0; i < frames; i++) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & (KEY_A | KEY_B | KEY_START)) break; // still skippable, just not required
        gspWaitForVBlank();
    }
}

void ui_wait_for_a(void) {
    // Several call sites (e.g. main.c's error-reporting paths) print then
    // wait_for_a without an explicit ui_flush() in between -- under the
    // old console-based ui.c, printf wrote straight into the framebuffer
    // so that happened to still show up; here nothing is actually drawn
    // until a frame is presented, so this guarantees one before blocking
    // instead of risking the text never becoming visible at all.
    ui_flush();
    while (true) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & (KEY_A | KEY_B | KEY_START)) break;
        gspWaitForVBlank();
    }
}

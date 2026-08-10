#include "ui.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
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

static u32 COLOR_BG;
static u32 COLOR_TEXT;
static u32 COLOR_MUTED;
static u32 COLOR_ACCENT;
static u32 COLOR_SUCCESS;
static u32 COLOR_DANGER;
static u32 COLOR_SELECT_BG;
static u32 COLOR_SELECT_TEXT;

typedef enum { LOG_TEXT, LOG_RULE } LogKind;

typedef struct {
    char text[256];
    u32 color;
    LogKind kind;
} LogLine;

#define MAX_LOG_LINES 200
static LogLine s_topLines[MAX_LOG_LINES];
static int s_topCount = 0;
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
} s_menu;

static const char *s_confirmPrompt;

void ui_init(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_textBuf = C2D_TextBufNew(TEXTBUF_GLYPHS);

    // A small, dark, teal-accented palette -- not trying to be a
    // pixel-exact clone of any particular app, just a reasonable-looking
    // starting point on top of real GPU rendering instead of the 16-color
    // console palette this replaces. Easy to retune once it's actually
    // visible on hardware; nothing else in this file depends on the
    // specific values.
    COLOR_BG          = C2D_Color32(0x16, 0x18, 0x1D, 0xFF);
    COLOR_TEXT        = C2D_Color32(0xE8, 0xEA, 0xED, 0xFF);
    COLOR_MUTED       = C2D_Color32(0x8A, 0x90, 0x9C, 0xFF);
    COLOR_ACCENT      = C2D_Color32(0x4F, 0xD1, 0xC5, 0xFF);
    COLOR_SUCCESS     = C2D_Color32(0x4C, 0xAF, 0x50, 0xFF);
    COLOR_DANGER      = C2D_Color32(0xE5, 0x39, 0x35, 0xFF);
    COLOR_SELECT_BG   = C2D_Color32(0x24, 0x3B, 0x3A, 0xFF);
    COLOR_SELECT_TEXT = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
}

void ui_exit(void) {
    C2D_TextBufDelete(s_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

static void log_append_line(LogLine *lines, int *count, const char *text, u32 color, LogKind kind) {
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
        log_append_line(lines, count, buf, color, LOG_TEXT);
        if (!nl) break;
        p = nl + 1;
    }
}

static void draw_log(const LogLine *lines, int count, float screenW) {
    int start = count > VISIBLE_LOG_LINES ? count - VISIBLE_LOG_LINES : 0;
    float y = MARGIN_Y;
    for (int i = start; i < count; i++) {
        const LogLine *line = &lines[i];
        if (line->kind == LOG_RULE) {
            C2D_DrawRectSolid(MARGIN_X, y + LINE_HEIGHT * 0.5f, 0.0f, screenW - 2 * MARGIN_X, 1.5f, line->color);
            y += LINE_HEIGHT;
            continue;
        }
        if (line->text[0] == '\0') { y += LINE_HEIGHT; continue; }

        C2D_Text t;
        C2D_TextParse(&t, s_textBuf, line->text);
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, MARGIN_X, y, 0.0f, TEXT_SCALE, TEXT_SCALE, line->color);
        y += LINE_HEIGHT;
    }
}

static void draw_menu(void) {
    float y = MARGIN_Y;

    C2D_Text titleText;
    C2D_TextParse(&titleText, s_textBuf, s_menu.title);
    C2D_TextOptimize(&titleText);
    C2D_DrawText(&titleText, C2D_WithColor, MARGIN_X, y, 0.0f, HEADER_SCALE, HEADER_SCALE, COLOR_ACCENT);
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

        C2D_Text itemText;
        C2D_TextParse(&itemText, s_textBuf, label ? label : "?");
        C2D_TextOptimize(&itemText);
        C2D_DrawText(&itemText, C2D_WithColor, MARGIN_X, y, 0.0f, TEXT_SCALE, TEXT_SCALE,
                     isSelected ? COLOR_SELECT_TEXT : COLOR_TEXT);
        y += LINE_HEIGHT;
    }

    y = BOTTOM_H - MARGIN_Y - LINE_HEIGHT;
    C2D_Text hint;
    C2D_TextParse(&hint, s_textBuf, "Up/Down select, A confirm, B cancel");
    C2D_TextOptimize(&hint);
    C2D_DrawText(&hint, C2D_WithColor, MARGIN_X, y, 0.0f, TEXT_SCALE, TEXT_SCALE, COLOR_MUTED);
}

static void draw_confirm(void) {
    float y = MARGIN_Y;

    C2D_Text promptText;
    C2D_TextParse(&promptText, s_textBuf, s_confirmPrompt);
    C2D_TextOptimize(&promptText);
    C2D_DrawText(&promptText, C2D_WithColor, MARGIN_X, y, 0.0f, HEADER_SCALE, HEADER_SCALE, COLOR_ACCENT);
    y += HEADER_HEIGHT;
    C2D_DrawRectSolid(MARGIN_X, y, 0.0f, BOTTOM_W - 2 * MARGIN_X, 1.5f, COLOR_ACCENT);
    y += LINE_HEIGHT * 1.5f;

    C2D_Text hint;
    C2D_TextParse(&hint, s_textBuf, "A = yes    B = no");
    C2D_TextOptimize(&hint);
    C2D_DrawText(&hint, C2D_WithColor, MARGIN_X, y, 0.0f, TEXT_SCALE, TEXT_SCALE, COLOR_MUTED);
}

static void present(ui_top_draw_fn drawTop, void *topUserdata) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    // Cleared once for the whole frame, not per line -- see
    // TEXTBUF_GLYPHS's comment. Both screens' text below shares this one
    // buffer.
    C2D_TextBufClear(s_textBuf);

    C2D_TargetClear(s_top, COLOR_BG);
    C2D_SceneBegin(s_top);
    if (drawTop) {
        drawTop(topUserdata);
    } else {
        draw_log(s_topLines, s_topCount, TOP_W);
    }

    C2D_TargetClear(s_bottom, COLOR_BG);
    C2D_SceneBegin(s_bottom);
    switch (s_bottomMode) {
        case BOTTOM_MENU:    draw_menu(); break;
        case BOTTOM_CONFIRM: draw_confirm(); break;
        default:              draw_log(s_bottomLines, s_bottomCount, BOTTOM_W); break;
    }

    C3D_FrameEnd(0);
}

void ui_flush(void) { present(NULL, NULL); }
void ui_flush_with_top(ui_top_draw_fn draw_top, void *userdata) { present(draw_top, userdata); }

void ui_clear(void) { s_topCount = 0; }

void ui_print(const char *text) { log_append(s_topLines, &s_topCount, text, COLOR_TEXT); }

void ui_printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ui_print(buf);
}

void ui_print_success(const char *text) { log_append(s_topLines, &s_topCount, text, COLOR_SUCCESS); }
void ui_print_error(const char *text) { log_append(s_topLines, &s_topCount, text, COLOR_DANGER); }

void ui_print_header(const char *title) {
    log_append_line(s_topLines, &s_topCount, title, COLOR_ACCENT, LOG_TEXT);
    log_append_line(s_topLines, &s_topCount, "", COLOR_ACCENT, LOG_RULE);
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
    log_append_line(s_bottomLines, &s_bottomCount, title, COLOR_ACCENT, LOG_TEXT);
    log_append_line(s_bottomLines, &s_bottomCount, "", COLOR_ACCENT, LOG_RULE);
}

int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata) {
    s_menu.title = title;
    s_menu.count = count;
    s_menu.getLabel = get_label;
    s_menu.userdata = userdata;
    s_menu.selected = 0;
    s_menu.topRow = 0;
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
            else if (kDown & KEY_B) { result = -1; goto done; }
            else gspWaitForVBlank();
        }
    }

done:
    s_bottomMode = BOTTOM_LOG;
    return result;
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

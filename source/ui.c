#include "ui.h"
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// libctru's console supports a small subset of ANSI SGR codes: reset (0),
// half-bright (2), reverse video (7), text color (30-37), background
// color (40-47) -- see 3ds-examples/graphics/printing/colored-text.
#define ANSI_RESET   "\x1b[0m"
#define ANSI_REVERSE "\x1b[7m"
#define ANSI_DIM     "\x1b[2m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_CYAN    "\x1b[36m"

// Bottom screen is 40 columns wide, top screen 50 -- used for the title
// underline so it spans the full width without hardcoding it twice.
#define BOTTOM_RULE "========================================"
#define TOP_RULE    "=================================================="

static PrintConsole s_topConsole;
static PrintConsole s_bottomConsole;

void ui_init(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &s_topConsole);
    consoleInit(GFX_BOTTOM, &s_bottomConsole);
    consoleSelect(&s_topConsole);
}

void ui_exit(void) {
    gfxExit();
}

void ui_clear(void) {
    consoleSelect(&s_topConsole);
    consoleClear();
}

void ui_print(const char *text) {
    consoleSelect(&s_topConsole);
    fputs(text, stdout);
}

void ui_printf(const char *fmt, ...) {
    consoleSelect(&s_topConsole);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void ui_print_success(const char *text) {
    consoleSelect(&s_topConsole);
    printf(ANSI_GREEN "%s" ANSI_RESET, text);
}

void ui_print_error(const char *text) {
    consoleSelect(&s_topConsole);
    printf(ANSI_RED "%s" ANSI_RESET, text);
}

void ui_print_header(const char *title) {
    consoleSelect(&s_topConsole);
    printf(ANSI_CYAN "%s" ANSI_RESET "\n%s\n\n", title, TOP_RULE);
}

void ui_flush(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata) {
    int selected = 0;
    int topRow = 0;
    const int visibleRows = 23; // leaves room for title/rule/footer on a 30-row console

    while (true) {
        consoleSelect(&s_bottomConsole);
        consoleClear();
        printf(ANSI_CYAN "%s" ANSI_RESET "\n%s\n\n", title, BOTTOM_RULE);

        if (selected < topRow) topRow = selected;
        if (selected >= topRow + visibleRows) topRow = selected - visibleRows + 1;

        for (int i = topRow; i < count && i < topRow + visibleRows; i++) {
            const char *label = get_label(i, userdata);
            if (i == selected) {
                printf(ANSI_REVERSE "> %s" ANSI_RESET "\n", label ? label : "?");
            } else {
                printf("  %s\n", label ? label : "?");
            }
        }
        printf(ANSI_DIM "\n(Up/Down select, A confirm, B cancel)" ANSI_RESET "\n");

        ui_flush();

        while (true) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_DOWN) { selected = (selected + 1) % count; break; }
            if (kDown & KEY_UP) { selected = (selected - 1 + count) % count; break; }
            if (kDown & KEY_A) { consoleSelect(&s_topConsole); return selected; }
            if (kDown & KEY_B) { consoleSelect(&s_topConsole); return -1; }
            gspWaitForVBlank();
        }
    }
}

bool ui_confirm(const char *prompt) {
    consoleSelect(&s_bottomConsole);
    consoleClear();
    printf(ANSI_CYAN "%s" ANSI_RESET "\n%s\n\n", prompt, BOTTOM_RULE);
    printf(ANSI_DIM "(A = yes, B = no)" ANSI_RESET "\n");
    ui_flush();

    bool result = false;
    while (true) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) { result = true; break; }
        if (kDown & KEY_B) { result = false; break; }
        gspWaitForVBlank();
    }
    consoleSelect(&s_topConsole);
    return result;
}

void ui_wait_for_a(void) {
    while (true) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & (KEY_A | KEY_B | KEY_START)) break;
        gspWaitForVBlank();
    }
}

void ui_clear_bottom(void) {
    consoleSelect(&s_bottomConsole);
    consoleClear();
    consoleSelect(&s_topConsole);
}

void ui_print_bottom(const char *text) {
    consoleSelect(&s_bottomConsole);
    fputs(text, stdout);
    consoleSelect(&s_topConsole);
}

void ui_printf_bottom(const char *fmt, ...) {
    consoleSelect(&s_bottomConsole);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    consoleSelect(&s_topConsole);
}

void ui_print_header_bottom(const char *title) {
    consoleSelect(&s_bottomConsole);
    printf(ANSI_CYAN "%s" ANSI_RESET "\n%s\n\n", title, BOTTOM_RULE);
    consoleSelect(&s_topConsole);
}

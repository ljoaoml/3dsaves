#include "ui.h"
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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

void ui_flush(void) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

int ui_run_menu(const char *title, int count, ui_menu_label_fn get_label, void *userdata) {
    int selected = 0;
    int topRow = 0;
    const int visibleRows = 25; // leaves room for title/footer on a 30-row console

    while (true) {
        consoleSelect(&s_bottomConsole);
        consoleClear();
        printf("%s\n\n", title);

        if (selected < topRow) topRow = selected;
        if (selected >= topRow + visibleRows) topRow = selected - visibleRows + 1;

        for (int i = topRow; i < count && i < topRow + visibleRows; i++) {
            const char *label = get_label(i, userdata);
            printf("%s %s\n", (i == selected) ? ">" : " ", label ? label : "?");
        }
        printf("\n(Up/Down select, A confirm, B cancel)\n");

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
    printf("%s\n\n(A = yes, B = no)\n", prompt);
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

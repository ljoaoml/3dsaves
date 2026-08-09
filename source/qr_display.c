#include "qr_display.h"
#include "qrcodegen.h"
#include "ui.h"

#include <3ds.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 240

// The Dropbox authorize URL is ~190 bytes, which needs roughly QR version
// 11-13 at low error correction -- version 20 is a comfortable ceiling
// that keeps the buffer (and qrcodegen's internal work) well under 1.2KB
// instead of the ~3.9KB qrcodegen_BUFFER_LEN_MAX (version 40) allows for.
#define QR_MAX_VERSION 20
#define QR_BUFFER_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)

// Pixel format/indexing verified against a known-working raw-framebuffer
// example (efairbanks/3ds-graphics-test, source/main.cpp): default format
// from gfxInitDefault() is BGR8 (3 bytes/pixel); despite the top screen's
// framebuffer being reported as 240(w) x 400(h) by gfxGetFramebuffer (the
// 90-degree-rotated raw layout), the correct index for on-screen (x,y)
// with x in [0,400), y in [0,240) is:
//   idx = (x * SCREEN_H + y) * 3
static void put_pixel(u8 *fb, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    int idx = (x * SCREEN_H + y) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

static void draw_qr_frame(const uint8_t *qr, int size, int scale, int originX, int originY) {
    // Force a known pixel format instead of trusting whatever the current
    // devkitPro/libctru version happens to default to -- the module count
    // diagnostic below confirmed the QR data itself is fine, so the bug is
    // in this drawing step; the framebuffer dimensions/format assumptions
    // below are unverified guesses, not confirmed facts, so print them
    // instead of asserting they're right.
    gfxSetScreenFormat(GFX_TOP, GFX_BGR8);
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memset(fb, 0xFF, SCREEN_W * SCREEN_H * 3); // white background = quiet zone

    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            if (!qrcodegen_getModule(qr, qx, qy)) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    put_pixel(fb, originX + qx * scale + dx, originY + qy * scale + dy,
                              0, 0, 0);
                }
            }
        }
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
}

int qr_display_and_wait(const char *text) {
    // qrcodegen's own buffers are ~3.9KB each (qrcodegen_BUFFER_LEN_MAX);
    // ~8KB combined is small in absolute terms, but a homebrew thread's
    // default stack can be small enough that two of these plus whatever
    // the caller's own call chain already used gets close to the edge.
    // Heap-allocating them removes stack size as a variable entirely --
    // a real (and cheap to rule out) suspect given the QR came out
    // corrupted identically both as a .3dsx and as a .cia, which pointed
    // away from anything screen/permission-specific and toward something
    // that corrupts the QR's *input data* before drawing even starts.
    uint8_t *qr = malloc(QR_BUFFER_LEN);
    uint8_t *tempBuffer = malloc(QR_BUFFER_LEN);
    if (!qr || !tempBuffer) {
        free(qr);
        free(tempBuffer);
        return -1;
    }

    bool ok = qrcodegen_encodeText(text, tempBuffer, qr, qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    free(tempBuffer);

    if (!ok) {
        free(qr);
        return -1;
    }

    int size = qrcodegen_getSize(qr);

    // Sanity print on the bottom screen: if this is ever obviously wrong
    // (not roughly 20-80), the corruption is happening before drawing.
    ui_printf_bottom("(QR: %d modules)\n", size);

    // What does THIS build's libctru actually report for the top screen's
    // raw framebuffer dimensions? Everything so far assumed 240x400
    // (rotated) -- if that's wrong on this devkitPro version, this line
    // will show it directly instead of us guessing again.
    u16 fbWidth = 0, fbHeight = 0;
    gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);
    ui_printf_bottom("(fb reports %dx%d)\n", fbWidth, fbHeight);

    if (size < 1 || size > 177) { // 177 = qrcodegen's absolute max (version 40)
        free(qr);
        return -1;
    }

    int scale = SCREEN_H / (size + 8); // leave ~4 modules of margin per side
    if (scale < 1) scale = 1;
    int qrPixels = size * scale;
    int originX = (SCREEN_W - qrPixels) / 2;
    int originY = (SCREEN_H - qrPixels) / 2;

    int result = -1;
    while (true) {
        // Redraw every frame (both buffers of the double-buffered top
        // screen need the QR painted into them, not just whichever one
        // gfxGetFramebuffer happened to hand back first).
        draw_qr_frame(qr, size, scale, originX, originY);

        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) { result = 1; break; }
        if (kDown & KEY_B) { result = 0; break; }
        gspWaitForVBlank();
    }

    free(qr);
    return result;
}

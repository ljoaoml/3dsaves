#include "qr_display.h"
#include "qrcodegen.h"

#include <3ds.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 240

// Pixel format/indexing verified against a known-working raw-framebuffer
// example (efairbanks/3ds-graphics-test, source/main.cpp): default format
// from gfxInitDefault() is BGR8 (3 bytes/pixel), and despite the top
// screen's framebuffer being reported as 240(w) x 400(h) by
// gfxGetFramebuffer (the 90-degree-rotated raw layout), the correct index
// for on-screen (x,y) with x in [0,400) and y in [0,240) is simply:
//   idx = (x * SCREEN_H + y) * 3
// An earlier version of this file used (SCREEN_H - 1 - y) to "un-flip" it
// and additionally called gfxSetDoubleBuffering(GFX_TOP, false) before
// drawing -- that combination produced a scrambled/garbled screen on real
// hardware. Removed the double-buffering toggle entirely and just redraw
// every frame while waiting (like the reference does for its animation),
// which keeps both of the double-buffered framebuffers correctly painted
// instead of relying on only one of them being "the" visible one.
static void put_pixel(u8 *fb, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    int idx = (x * SCREEN_H + y) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

static void draw_qr_frame(const uint8_t *qr, int size, int scale, int originX, int originY) {
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
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(text, tempBuffer, qr, qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                    qrcodegen_Mask_AUTO, true);
    if (!ok) return -1;

    int size = qrcodegen_getSize(qr);
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

    return result;
}

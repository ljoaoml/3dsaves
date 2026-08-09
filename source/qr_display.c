#include "qr_display.h"
#include "qrcodegen.h"

#include <3ds.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 240

// The 3DS top screen's raw framebuffer is stored rotated: gfxGetFramebuffer
// reports it as 240(w) x 400(h), addressed column-by-column of the *normal*
// (400x240 landscape) image. To plot a pixel at normal on-screen (x,y) with
// x in [0,400) and y in [0,240), the byte offset (3 bytes/pixel, BGR8, the
// default format from gfxInitDefault) is:
//   idx = (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3
// This is the standard, widely-used formula for raw pixel access on 3DS;
// getting it wrong produces a sideways/mirrored image rather than a crash,
// so if the QR ever looks scrambled on hardware, this is the first thing
// to double check.
static void put_pixel(u8 *fb, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    int idx = (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

int qr_display_and_wait(const char *text) {
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(text, tempBuffer, qr, qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                    qrcodegen_Mask_AUTO, true);
    if (!ok) return -1;

    int size = qrcodegen_getSize(qr);

    // Static image: turn off double buffering so what we draw just stays
    // on screen without needing to redraw every frame.
    gfxSetDoubleBuffering(GFX_TOP, false);
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);

    memset(fb, 0xFF, SCREEN_W * SCREEN_H * 3); // white background = quiet zone

    int scale = SCREEN_H / (size + 8); // leave ~4 modules of margin per side
    if (scale < 1) scale = 1;
    int qrPixels = size * scale;
    int originX = (SCREEN_W - qrPixels) / 2;
    int originY = (SCREEN_H - qrPixels) / 2;

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

    int result = -1;
    while (true) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) { result = 1; break; }
        if (kDown & KEY_B) { result = 0; break; }
        gspWaitForVBlank();
    }

    // Restore normal double-buffered state for the text console.
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSwapBuffers();

    return result;
}

#include "qr_display.h"
#include "qrcodegen.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 400
#define SCREEN_H 240

// The Dropbox authorize URL is ~250 bytes now that it includes a
// redirect_uri + state, which needs roughly QR version 14-16 at low error
// correction -- version 20 stays a comfortable ceiling.
#define QR_MAX_VERSION 20
#define QR_BUFFER_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)

struct QrCode {
    uint8_t *data;
    int size;
    int scale;
    int originX;
    int originY;
};

// Pixel format/indexing: confirmed correct on real hardware by explicitly
// forcing GSP_BGR8_OES (3 bytes/pixel) instead of trusting whatever this
// devkitPro version's default happens to be, and using the plain (no
// vertical flip) index despite the top screen's framebuffer being
// reported as 240(w) x 400(h) by gfxGetFramebuffer (the 90-degree-rotated
// raw layout):
//   idx = (x * SCREEN_H + y) * 3
static void put_pixel(u8 *fb, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    int idx = (x * SCREEN_H + y) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

QrCode *qr_prepare(const char *text) {
    uint8_t *data = malloc(QR_BUFFER_LEN);
    uint8_t *tempBuffer = malloc(QR_BUFFER_LEN);
    if (!data || !tempBuffer) {
        free(data);
        free(tempBuffer);
        return NULL;
    }

    bool ok = qrcodegen_encodeText(text, tempBuffer, data, qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    free(tempBuffer);
    if (!ok) {
        free(data);
        return NULL;
    }

    int size = qrcodegen_getSize(data);
    if (size < 1 || size > 177) {
        free(data);
        return NULL;
    }

    QrCode *qr = malloc(sizeof(QrCode));
    if (!qr) {
        free(data);
        return NULL;
    }

    qr->data = data;
    qr->size = size;
    qr->scale = SCREEN_H / (size + 8); // leave ~4 modules of margin per side
    if (qr->scale < 1) qr->scale = 1;
    int qrPixels = size * qr->scale;
    qr->originX = (SCREEN_W - qrPixels) / 2;
    qr->originY = (SCREEN_H - qrPixels) / 2;

    return qr;
}

void qr_draw_frame(const QrCode *qr) {
    if (!qr) return;

    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memset(fb, 0xFF, SCREEN_W * SCREEN_H * 3); // white background = quiet zone

    for (int qy = 0; qy < qr->size; qy++) {
        for (int qx = 0; qx < qr->size; qx++) {
            if (!qrcodegen_getModule(qr->data, qx, qy)) continue;
            for (int dy = 0; dy < qr->scale; dy++) {
                for (int dx = 0; dx < qr->scale; dx++) {
                    put_pixel(fb, qr->originX + qx * qr->scale + dx,
                              qr->originY + qy * qr->scale + dy, 0, 0, 0);
                }
            }
        }
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
}

void qr_free(QrCode *qr) {
    if (!qr) return;
    free(qr->data);
    free(qr);
}

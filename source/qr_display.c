#include "qr_display.h"
#include "qrcodegen.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>

// citro2d's logical top-screen coordinate space: 400x240, (0,0) at the
// top-left in normal (non-rotated) orientation. Unlike the old raw
// framebuffer version, there's no manual rotation/byte-layout math here
// -- citro2d handles the physical screen's rotated memory layout
// internally, draw calls just use plain logical (x, y).
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

void qr_draw_frame(void *userdata) {
    const QrCode *qr = (const QrCode *)userdata;
    if (!qr) return;

    // White background covering the whole screen first: a QR's "quiet
    // zone" (light margin around the code) matters for real-world
    // scanner reliability, and this is drawn over ui.c's own dark theme
    // background (already cleared by present() before this callback
    // runs), not a blank slate.
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H,
                       C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

    u32 black = C2D_Color32(0x00, 0x00, 0x00, 0xFF);
    for (int qy = 0; qy < qr->size; qy++) {
        for (int qx = 0; qx < qr->size; qx++) {
            if (!qrcodegen_getModule(qr->data, qx, qy)) continue;
            C2D_DrawRectSolid((float)(qr->originX + qx * qr->scale),
                               (float)(qr->originY + qy * qr->scale),
                               0.0f, (float)qr->scale, (float)qr->scale, black);
        }
    }
}

void qr_free(QrCode *qr) {
    if (!qr) return;
    free(qr->data);
    free(qr);
}

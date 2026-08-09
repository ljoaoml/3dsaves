#pragma once

// Draws a QR code directly to the top screen's framebuffer (not through
// the text console). Split into prepare/draw-one-frame/free so a caller
// can redraw it every loop iteration while also doing other work (like
// polling a server) in between frames, instead of blocking entirely.

typedef struct QrCode QrCode;

// Encodes `text` as a QR code. Returns NULL if it couldn't be encoded
// (caller should fall back to not showing a QR at all).
QrCode *qr_prepare(const char *text);

// Draws one frame of the QR to the top screen and presents it. Call this
// every iteration of your own loop (it needs to be redrawn every frame
// because the top screen is double-buffered -- see qr_display.c).
void qr_draw_frame(const QrCode *qr);

void qr_free(QrCode *qr);

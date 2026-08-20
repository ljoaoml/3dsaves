#pragma once

// Draws a QR code via citro2d (see source/qr_display.c), as a grid of
// solid-color rectangles -- one per "on" module -- rather than a
// texture/image, to avoid needing the C3D_Tex/C2D_Image pixel-format
// path for what's fundamentally a two-color grid. Split into
// prepare/draw-one-frame/free so a caller can redraw it every loop
// iteration while also doing other work (like checking for a pasted
// token file) in between frames, instead of blocking entirely.

typedef struct QrCode QrCode;

// Encodes `text` as a QR code. Returns NULL if it couldn't be encoded
// (caller should fall back to not showing a QR at all).
QrCode *qr_prepare(const char *text);

// Draws one frame of the QR into the *currently active* citro2d scene --
// call this as the draw_top callback of ui_flush_with_top(qr_draw_frame,
// qr), not on its own; it doesn't begin/end a frame or present anything
// itself (unlike the old raw-framebuffer version), since there's only
// one GPU frame per vblank and ui.c's present() already needs the rest
// of that frame for the bottom screen. `userdata` is the QrCode* from
// qr_prepare(), cast back internally -- matches ui_top_draw_fn's
// signature so it can be passed directly.
void qr_draw_frame(void *userdata);

void qr_free(QrCode *qr);

#pragma once

// Draws a QR code of `text` directly to the top screen's framebuffer (not
// through the text console) and waits for a button press. Used so the
// Dropbox OAuth URL -- too long to type from the 3DS's tiny screen -- can
// just be scanned with a phone camera instead.
//
// Returns 1 if the user pressed A, 0 if they pressed B (cancel), or -1 if
// the QR code couldn't be generated at all (caller should fall back to
// showing the URL as plain text only).
int qr_display_and_wait(const char *text);

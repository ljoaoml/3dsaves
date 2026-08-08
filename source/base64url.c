#include "base64url.h"

static const char s_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t base64url_encoded_len(size_t len) {
    return (len + 2) / 3 * 4;
}

void base64url_encode(const uint8_t *data, size_t len, char *out) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = s_table[(n >> 18) & 0x3F];
        out[o++] = s_table[(n >> 12) & 0x3F];
        out[o++] = s_table[(n >> 6) & 0x3F];
        out[o++] = s_table[n & 0x3F];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = s_table[(n >> 18) & 0x3F];
        out[o++] = s_table[(n >> 12) & 0x3F];
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = s_table[(n >> 18) & 0x3F];
        out[o++] = s_table[(n >> 12) & 0x3F];
        out[o++] = s_table[(n >> 6) & 0x3F];
    }
    out[o] = '\0'; // unpadded, per RFC 7636 PKCE requirements
}

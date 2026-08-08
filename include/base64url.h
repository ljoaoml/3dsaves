#pragma once
#include <stddef.h>
#include <stdint.h>

// Encodes `len` bytes as unpadded base64url (RFC 4648 sec. 5) into `out`,
// which must be at least base64url_encoded_len(len)+1 bytes.
// Used for PKCE code_verifier/code_challenge, which both require this
// encoding rather than standard base64.
size_t base64url_encoded_len(size_t len);
void base64url_encode(const uint8_t *data, size_t len, char *out);

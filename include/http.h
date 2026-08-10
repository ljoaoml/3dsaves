#pragma once
#include <3ds.h>
#include <stdio.h>

// Thin wrapper around libctru's httpc service: TLS setup (default + bundled
// root CAs), header/body handling, redirect following and response buffering.

typedef struct {
    u8 *data;
    u32 size;
} HttpBuffer;

typedef struct {
    const char *name;
    const char *value;
} HttpHeader;

typedef struct {
    u32 status_code;
    HttpBuffer body;
} HttpResponse;

// Loads the bundled root CA certs from romfs. Call once after romfsInit().
Result http_init(void);
void http_exit(void);

// Diagnostics for the bundled-cert trust setup (see romfs/certs/README.md
// for why this exists at all): how many cert files were actually read
// from romfs vs. how many are compiled in, and how many of those were
// successfully registered as trusted roots on the most recent request.
// Use these to tell apart "romfs is missing files" from "httpc rejected
// some certs" from "certs are all fine, the failure is something else".
int http_get_loaded_cert_count(void);
int http_get_total_cert_count(void);
int http_get_last_trusted_count(void);

// Diagnostics for the most recent request's TLS write/read progress, set
// at the start of every request (before the connection attempt) and
// updated as it goes: -1 means "never got that far" (e.g. request bytes
// sent stays -1 if the handshake itself never completed), 0+ is an
// actual byte count. Lets a failing request be told apart as "nothing
// was ever sent", "sent fine but zero bytes of response ever came back"
// (looks identical to a hung connection from the Result code alone) or
// "some response arrived, then it stalled".
int http_get_last_request_bytes_sent(void);
int http_get_last_response_bytes_received(void);

// Which IP the most recent request's hostname resolved to, and what TLS
// version/cipher suite got negotiated (or "?" if not gotten that far).
// Lets a real-hardware result be compared directly against e.g.
// `openssl s_client` run from a PC on the same network. A request that
// fails outright, or comes back with a non-2xx status, also (over)writes
// sdmc:/3ds/Konnect3DS/http_debug.log with this plus the exact
// request/response bytes -- a successful request doesn't touch it at all,
// so its mere presence already means something went wrong; check it after
// anything that fails in a way that's hard to diagnose from just the
// Result code and these getters.
const char *http_get_last_resolved_ip(void);
const char *http_get_last_tls_version(void);
const char *http_get_last_tls_cipher(void);

// If the most recent request failed specifically because the server's
// TLS certificate didn't verify, this is mbedTLS's human-readable reason
// why (untrusted CA, expired, hostname mismatch, ...) -- "?" otherwise.
const char *http_get_last_verify_info(void);

// Raw mbedtls_ssl_get_verify_result() bitmask from the most recent
// connection attempt, before (raw) and after (after_mask) the
// date-related flags (BADCERT_FUTURE, BADCERT_EXPIRED) are cleared.
// 0xFFFFFFFF means no connection attempt got far enough to check.
u32 http_get_last_verify_flags_raw(void);
u32 http_get_last_verify_flags_after_mask(void);

// Performs a single HTTPS request, following redirects (up to 5 hops).
// `body`/`body_size` may be NULL/0 for methods without a request body.
// On success fills `out` (caller must call http_response_free when done).
Result http_request(HTTPC_RequestMethod method, const char *url,
                     const HttpHeader *headers, int header_count,
                     const u8 *body, u32 body_size,
                     HttpResponse *out);

// Same as http_request but the body is read from an already-open file.
// libctru's httpc raw POST API wants the full body up front, so this still
// buffers `body_size` bytes in RAM before sending -- fine for typical save
// sizes, but a real streaming upload would need chunked/multi-part support
// that httpc does not expose cleanly. Keep an eye on this for very large
// (tens of MB) extdata saves.
Result http_request_file_body(HTTPC_RequestMethod method, const char *url,
                               const HttpHeader *headers, int header_count,
                               FILE *body_file, u32 body_size,
                               HttpResponse *out);

void http_response_free(HttpResponse *resp);

// URL-encodes `src` into `dst` (dst must be large enough; worst case 3x).
void http_url_encode(const char *src, char *dst, size_t dst_size);

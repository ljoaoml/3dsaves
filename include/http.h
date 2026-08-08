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

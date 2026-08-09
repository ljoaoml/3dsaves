#include "http.h"
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_REDIRECTS 5
#define HTTP_CHUNK_SIZE (64 * 1024)

// Bundled root CA certs (DER), loaded from romfs at startup. These cover
// Dropbox's current/recently-rotated TLS chains plus the other major
// public CAs (Google Trust Services for the Cloudflare relay's
// *.workers.dev, Sectigo/USERTrust, Amazon) rather than depending on the
// console's aging built-in cert list. See romfs/certs/README.md.
static const char *const s_certFiles[] = {
    "romfs:/certs/isrg_root_x1.der",
    "romfs:/certs/digicert_global_root_g2.der",
    "romfs:/certs/digicert_global_root_g3.der",
    "romfs:/certs/digicert_tls_rsa4096_root_g5.der",
    "romfs:/certs/digicert_tls_ecc_p384_root_g5.der",
    "romfs:/certs/gts_root_r1.der",
    "romfs:/certs/gts_root_r4.der",
    "romfs:/certs/usertrust_rsa.der",
    "romfs:/certs/amazon_root_ca_1.der",
};
#define NUM_CERT_FILES (sizeof(s_certFiles) / sizeof(s_certFiles[0]))

typedef struct {
    u8 *data;
    u32 size;
} LoadedCert;

static LoadedCert s_certs[NUM_CERT_FILES];
static int s_certCount = 0;
static bool s_httpInited = false;

Result http_init(void) {
    Result rc = httpcInit(HTTP_CHUNK_SIZE);
    if (R_FAILED(rc)) return rc;

    for (size_t i = 0; i < NUM_CERT_FILES; i++) {
        FILE *f = fopen(s_certFiles[i], "rb");
        if (!f) continue; // romfs missing this cert; skip, not fatal
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        u8 *buf = malloc((size_t)sz);
        if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf);
            fclose(f);
            continue;
        }
        fclose(f);
        s_certs[s_certCount].data = buf;
        s_certs[s_certCount].size = (u32)sz;
        s_certCount++;
    }

    s_httpInited = true;
    return 0;
}

void http_exit(void) {
    for (int i = 0; i < s_certCount; i++) free(s_certs[i].data);
    s_certCount = 0;
    if (s_httpInited) httpcExit();
    s_httpInited = false;
}

void http_url_encode(const char *src, char *dst, size_t dst_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *src && o + 4 < dst_size; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0xF];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

static Result apply_ssl_trust(httpcContext *ctx) {
    Result rc = 0;
    // A couple of Nintendo-firmware built-in roots that also happen to
    // cover common CDN/API fronting; harmless to add even if unused.
    httpcAddDefaultCert(ctx, SSLC_DefaultRootCert_CyberTrust);
    httpcAddDefaultCert(ctx, SSLC_DefaultRootCert_DigiCert_EV);
    for (int i = 0; i < s_certCount; i++) {
        rc = httpcAddTrustedRootCA(ctx, s_certs[i].data, s_certs[i].size);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

static Result buffer_append(HttpBuffer *buf, const u8 *chunk, u32 chunk_size) {
    u8 *newData = realloc(buf->data, buf->size + chunk_size + 1);
    if (!newData) return -1;
    memcpy(newData + buf->size, chunk, chunk_size);
    buf->data = newData;
    buf->size += chunk_size;
    buf->data[buf->size] = '\0'; // keep it usable as a C string for JSON bodies
    return 0;
}

static Result download_body(httpcContext *ctx, HttpBuffer *out) {
    memset(out, 0, sizeof(*out));
    u8 chunk[HTTP_CHUNK_SIZE];
    Result rc;
    u32 downloaded = 0;
    do {
        rc = httpcDownloadData(ctx, chunk, sizeof(chunk), &downloaded);
        if (downloaded > 0) {
            if (R_FAILED(buffer_append(out, chunk, downloaded))) {
                return -1;
            }
        }
    } while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    return rc;
}

static Result do_single_request(HTTPC_RequestMethod method, const char *url,
                                 const HttpHeader *headers, int header_count,
                                 const u8 *body, u32 body_size,
                                 char *redirect_out, size_t redirect_out_size,
                                 HttpResponse *out) {
    httpcContext ctx;
    Result rc = httpcOpenContext(&ctx, method, url, 0);
    if (R_FAILED(rc)) return rc;

    rc = apply_ssl_trust(&ctx);
    if (R_FAILED(rc)) { httpcCloseContext(&ctx); return rc; }

    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);

    for (int i = 0; i < header_count; i++) {
        httpcAddRequestHeaderField(&ctx, headers[i].name, headers[i].value);
    }

    if (body && body_size > 0) {
        rc = httpcAddPostDataRaw(&ctx, (const u32 *)body, body_size);
        if (R_FAILED(rc)) { httpcCloseContext(&ctx); return rc; }
    }

    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) { httpcCloseContext(&ctx); return rc; }

    u32 status = 0;
    rc = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(rc)) { httpcCloseContext(&ctx); return rc; }

    if ((status >= 301 && status <= 303) || status == 307 || status == 308) {
        if (redirect_out) {
            Result lrc = httpcGetResponseHeader(&ctx, "Location", redirect_out, redirect_out_size);
            if (R_SUCCEEDED(lrc)) {
                out->status_code = status;
                httpcCloseContext(&ctx);
                return (Result)0x1; // sentinel: caller should redirect
            }
        }
    }

    HttpBuffer buf;
    rc = download_body(&ctx, &buf);
    httpcCloseContext(&ctx);
    if (R_FAILED(rc)) return rc;

    out->status_code = status;
    out->body = buf;
    return 0;
}

Result http_request(HTTPC_RequestMethod method, const char *url,
                     const HttpHeader *headers, int header_count,
                     const u8 *body, u32 body_size,
                     HttpResponse *out) {
    if (!s_httpInited) return -1;
    memset(out, 0, sizeof(*out));

    char currentUrl[1024];
    strncpy(currentUrl, url, sizeof(currentUrl) - 1);
    currentUrl[sizeof(currentUrl) - 1] = '\0';

    for (int hop = 0; hop < HTTP_MAX_REDIRECTS; hop++) {
        char location[1024] = {0};
        Result rc = do_single_request(method, currentUrl, headers, header_count,
                                       body, body_size,
                                       location, sizeof(location), out);
        if (rc == (Result)0x1) {
            strncpy(currentUrl, location, sizeof(currentUrl) - 1);
            currentUrl[sizeof(currentUrl) - 1] = '\0';
            continue;
        }
        return rc;
    }
    return -1; // too many redirects
}

Result http_request_file_body(HTTPC_RequestMethod method, const char *url,
                               const HttpHeader *headers, int header_count,
                               FILE *body_file, u32 body_size,
                               HttpResponse *out) {
    if (!s_httpInited) return -1;
    memset(out, 0, sizeof(*out));

    u8 *buf = malloc(body_size);
    if (!buf) return -1;
    rewind(body_file);
    if (fread(buf, 1, body_size, body_file) != body_size) {
        free(buf);
        return -1;
    }

    Result rc = http_request(method, url, headers, header_count, buf, body_size, out);
    free(buf);
    return rc;
}

void http_response_free(HttpResponse *resp) {
    if (resp->body.data) free(resp->body.data);
    resp->body.data = NULL;
    resp->body.size = 0;
}

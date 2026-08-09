#include "http.h"
#include <3ds.h>
#include <3ds/services/soc.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

// The 3DS's own httpc/ssl:C stack cannot complete a TLS handshake with any
// modern server (confirmed: fails identically against Cloudflare and
// Dropbox's own servers, and even the console's native Internet Browser
// fails the same way against google.com/dropbox.com with a generic
// "update your browser" error -- this is a known, documented limitation,
// the same reason FBI ships its own TLS stack). This file instead opens
// raw sockets (soc:u) and speaks TLS itself via mbedTLS, building HTTP/1.1
// requests/responses by hand. The public API (http.h) is unchanged so
// auth.c/dropbox.c didn't need to change.

#define HTTP_MAX_REDIRECTS 5
#define HTTP_RECV_CHUNK 4096
#define SOC_BUFFER_SIZE (128 * 1024)
#define SOC_ALIGN 0x1000

// Same bundled root CA set as before (see romfs/certs/README.md for how
// each one was verified) -- still needed, just parsed by mbedTLS now
// instead of handed to httpc.
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
    "romfs:/certs/sslcom_root_rsa.der",
    "romfs:/certs/sslcom_root_ecc.der",
    "romfs:/certs/sslcom_tls_rsa_root_2022.der",
    "romfs:/certs/sslcom_tls_ecc_root_2022.der",
    "romfs:/certs/globalsign_ecc_root_r4.der",
    "romfs:/certs/globalsign_ecc_root_r5.der",
    "romfs:/certs/globalsign_root_r3.der",
    "romfs:/certs/globalsign_root_r6.der",
};
#define NUM_CERT_FILES (sizeof(s_certFiles) / sizeof(s_certFiles[0]))

// Fake Result codes for failures that don't come from libctru (the top
// bit alone is what R_FAILED checks). Low bits carry an mbedtls error
// code (negated) or a small local reason, so a failing rc printed as
// %08lX is still greppable against mbedtls's own error space.
#define HTTP_ERR_BASE 0x80AD0000
#define HTTP_ERR_BAD_URL      ((Result)(HTTP_ERR_BASE | 0x0001))
#define HTTP_ERR_TOO_MANY_REDIRECTS ((Result)(HTTP_ERR_BASE | 0x0002))
#define HTTP_ERR_BAD_RESPONSE ((Result)(HTTP_ERR_BASE | 0x0003))
#define HTTP_ERR_OOM          ((Result)(HTTP_ERR_BASE | 0x0004))
#define HTTP_REDIRECT_SENTINEL ((Result)0x1)

// Distinct synthetic "mbedtls error codes" (never real ones -- picked
// well outside any real module's range) for our own deadline hits in
// tls_connect/tls_write_all/tls_read_some, all of which would otherwise
// collapse to the same MBEDTLS_ERR_SSL_TIMEOUT and be indistinguishable
// from the printed rc alone.
#define TLS_ERR_HANDSHAKE_TIMEOUT (-0x7001)
#define TLS_ERR_WRITE_TIMEOUT     (-0x7002)
#define TLS_ERR_READ_TIMEOUT      (-0x7003)

static Result mbedtls_to_result(int mbedErr) {
    u32 code = (u32)((mbedErr < 0) ? -mbedErr : mbedErr) & 0xFFFF;
    return (Result)(HTTP_ERR_BASE | code);
}

static u32 *s_socBuffer = NULL;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctrDrbg;
static mbedtls_x509_crt s_caChain;
static bool s_httpInited = false;
static int s_certFileCount = 0;   // .der files actually read from romfs
static int s_certParsedCount = 0; // of those, how many mbedtls accepted

int http_get_loaded_cert_count(void) { return s_certFileCount; }
int http_get_total_cert_count(void) { return (int)NUM_CERT_FILES; }
int http_get_last_trusted_count(void) { return s_certParsedCount; }

Result http_init(void) {
    s_socBuffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFER_SIZE);
    if (!s_socBuffer) return HTTP_ERR_OOM;

    Result rc = socInit(s_socBuffer, SOC_BUFFER_SIZE);
    if (R_FAILED(rc)) {
        free(s_socBuffer);
        s_socBuffer = NULL;
        return rc;
    }

    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctrDrbg);
    static const char *const pers = "Konnect3DS";
    int ret = mbedtls_ctr_drbg_seed(&s_ctrDrbg, mbedtls_entropy_func, &s_entropy,
                                     (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        mbedtls_ctr_drbg_free(&s_ctrDrbg);
        mbedtls_entropy_free(&s_entropy);
        socExit();
        free(s_socBuffer);
        s_socBuffer = NULL;
        return mbedtls_to_result(ret);
    }

    mbedtls_x509_crt_init(&s_caChain);
    for (size_t i = 0; i < NUM_CERT_FILES; i++) {
        FILE *f = fopen(s_certFiles[i], "rb");
        if (!f) continue; // romfs missing this cert; skip, not fatal
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        u8 *buf = malloc((size_t)sz);
        if (!buf) { fclose(f); continue; }
        size_t readBytes = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (readBytes != (size_t)sz) { free(buf); continue; }
        s_certFileCount++;
        if (mbedtls_x509_crt_parse_der(&s_caChain, buf, (size_t)sz) == 0) {
            s_certParsedCount++;
        }
        free(buf); // mbedtls copies the DER data internally
    }

    s_httpInited = true;
    return 0;
}

void http_exit(void) {
    mbedtls_x509_crt_free(&s_caChain);
    mbedtls_ctr_drbg_free(&s_ctrDrbg);
    mbedtls_entropy_free(&s_entropy);
    if (s_httpInited) socExit();
    if (s_socBuffer) { free(s_socBuffer); s_socBuffer = NULL; }
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

static Result buffer_append(HttpBuffer *buf, const u8 *chunk, u32 chunk_size) {
    u8 *newData = realloc(buf->data, buf->size + chunk_size + 1);
    if (!newData) return HTTP_ERR_OOM;
    memcpy(newData + buf->size, chunk, chunk_size);
    buf->data = newData;
    buf->size += chunk_size;
    buf->data[buf->size] = '\0'; // keep it usable as a C string for JSON bodies/headers
    return 0;
}

// ---- URL parsing -----------------------------------------------------

typedef struct {
    bool https;
    char host[256];
    int port;
    char path[1024];
} ParsedUrl;

static bool parse_url(const char *url, ParsedUrl *out) {
    memset(out, 0, sizeof(*out));

    const char *hostStart;
    if (strncmp(url, "https://", 8) == 0) {
        out->https = true;
        out->port = 443;
        hostStart = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->https = false;
        out->port = 80;
        hostStart = url + 7;
    } else {
        return false;
    }
    if (*hostStart == '\0') return false;

    const char *pathStart = strchr(hostStart, '/');
    const char *hostEnd = pathStart ? pathStart : hostStart + strlen(hostStart);
    const char *portSep = memchr(hostStart, ':', (size_t)(hostEnd - hostStart));
    const char *hostNameEnd = portSep ? portSep : hostEnd;

    size_t hostLen = (size_t)(hostNameEnd - hostStart);
    if (hostLen == 0 || hostLen >= sizeof(out->host)) return false;
    memcpy(out->host, hostStart, hostLen);
    out->host[hostLen] = '\0';

    if (portSep) {
        char portBuf[8] = {0};
        size_t portLen = (size_t)(hostEnd - (portSep + 1));
        if (portLen == 0 || portLen >= sizeof(portBuf)) return false;
        memcpy(portBuf, portSep + 1, portLen);
        out->port = atoi(portBuf);
    }

    if (pathStart) {
        snprintf(out->path, sizeof(out->path), "%s", pathStart);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }
    return true;
}

static const char *method_to_string(HTTPC_RequestMethod method) {
    switch (method) {
        case HTTPC_METHOD_GET:    return "GET";
        case HTTPC_METHOD_POST:   return "POST";
        case HTTPC_METHOD_PUT:    return "PUT";
        case HTTPC_METHOD_DELETE: return "DELETE";
        case HTTPC_METHOD_HEAD:   return "HEAD";
        default:                  return "GET";
    }
}

// ---- TLS connection ----------------------------------------------------

// Blocking sockets with no timeout can hang the whole app forever on a
// stalled network op (no response, half-open connection, etc.) -- 3DS
// apps have no OS-level "kill hung app" safety net, so this must be
// bounded. HTTP_IO_TIMEOUT_MS caps every post-connect wait (TLS
// handshake, write, read) via a wall-clock deadline checked around the
// WANT_READ/WANT_WRITE retry loops.
#define HTTP_IO_TIMEOUT_MS 15000

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
} TlsConn;

static bool deadline_passed(u64 startMs, u32 timeoutMs) {
    return (osGetTime() - startMs) > timeoutMs;
}

static Result tls_connect(const char *host, int port, TlsConn *c) {
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);

    int ret = mbedtls_net_connect(&c->net, host, portStr, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) goto fail;

    ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) goto fail;

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &s_caChain, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &s_ctrDrbg);
    mbedtls_ssl_conf_read_timeout(&c->conf, HTTP_IO_TIMEOUT_MS);

    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) goto fail;

    ret = mbedtls_ssl_set_hostname(&c->ssl, host); // SNI + CN/SAN check target
    if (ret != 0) goto fail;

    // mbedtls_net_recv_timeout (select()-based, part of mbedtls itself --
    // not relying on the platform's setsockopt(SO_RCVTIMEO) support, which
    // isn't guaranteed here) bounds every read; combined with
    // mbedtls_ssl_conf_read_timeout above, a stalled peer now surfaces as
    // MBEDTLS_ERR_SSL_TIMEOUT instead of blocking forever.
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    {
        u64 start = osGetTime();
        while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
            // mbedtls_net_recv_timeout (wired up as f_recv_timeout below)
            // makes a stalled read surface as MBEDTLS_ERR_SSL_TIMEOUT
            // directly from mbedtls_ssl_handshake() itself -- this is the
            // common case, and it bypasses the WANT_READ/WANT_WRITE retry
            // path entirely, so it has to be caught here too, not just via
            // deadline_passed() below.
            if (ret == MBEDTLS_ERR_SSL_TIMEOUT) { ret = TLS_ERR_HANDSHAKE_TIMEOUT; goto fail; }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
            if (deadline_passed(start, HTTP_IO_TIMEOUT_MS)) { ret = TLS_ERR_HANDSHAKE_TIMEOUT; goto fail; }
        }
    }

    return 0;

fail:
    {
        Result rc = mbedtls_to_result(ret);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_net_free(&c->net);
        return rc;
    }
}

static void tls_close(TlsConn *c) {
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_net_free(&c->net);
}

static int tls_write_all(mbedtls_ssl_context *ssl, const u8 *data, size_t len) {
    size_t off = 0;
    u64 start = osGetTime();
    while (off < len) {
        int ret = mbedtls_ssl_write(ssl, data + off, len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (deadline_passed(start, HTTP_IO_TIMEOUT_MS)) return TLS_ERR_WRITE_TIMEOUT;
            continue;
        }
        if (ret <= 0) return ret;
        off += (size_t)ret;
        start = osGetTime(); // reset the deadline on forward progress
    }
    return 0;
}

// >0 = bytes read, 0 = clean EOF, <0 = hard error
static int tls_read_some(mbedtls_ssl_context *ssl, u8 *buf, size_t len) {
    int ret;
    u64 start = osGetTime();
    do {
        ret = mbedtls_ssl_read(ssl, buf, len);
        // See the matching comment in tls_connect(): a stalled read
        // surfaces as MBEDTLS_ERR_SSL_TIMEOUT directly, not as a
        // WANT_READ/WANT_WRITE loop we'd catch below.
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) return TLS_ERR_READ_TIMEOUT;
        if ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            deadline_passed(start, HTTP_IO_TIMEOUT_MS)) {
            return TLS_ERR_READ_TIMEOUT;
        }
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_NET_CONN_RESET) return 0;
    return ret;
}

// ---- HTTP/1.1 framing --------------------------------------------------

static const u8 *find_bytes(const u8 *hay, size_t hayLen, const char *needle) {
    size_t needleLen = strlen(needle);
    if (needleLen == 0 || hayLen < needleLen) return NULL;
    for (size_t i = 0; i + needleLen <= hayLen; i++) {
        if (memcmp(hay + i, needle, needleLen) == 0) return hay + i;
    }
    return NULL;
}

static Result decode_chunked(const u8 *data, size_t len, HttpBuffer *out) {
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    while (pos < len) {
        const u8 *crlf = find_bytes(data + pos, len - pos, "\r\n");
        if (!crlf) return HTTP_ERR_BAD_RESPONSE;
        size_t lineLen = (size_t)(crlf - (data + pos));
        char sizeBuf[16];
        if (lineLen == 0 || lineLen >= sizeof(sizeBuf)) return HTTP_ERR_BAD_RESPONSE;
        memcpy(sizeBuf, data + pos, lineLen);
        sizeBuf[lineLen] = '\0';
        char *semi = strchr(sizeBuf, ';');
        if (semi) *semi = '\0';

        char *end = NULL;
        long chunkSize = strtol(sizeBuf, &end, 16);
        if (end == sizeBuf || chunkSize < 0) return HTTP_ERR_BAD_RESPONSE;

        pos += lineLen + 2;
        if (chunkSize == 0) break; // final chunk; ignore any trailers

        if (pos + (size_t)chunkSize > len) return HTTP_ERR_BAD_RESPONSE;
        Result rc = buffer_append(out, data + pos, (u32)chunkSize);
        if (R_FAILED(rc)) return rc;
        pos += (size_t)chunkSize;

        if (pos + 2 <= len && data[pos] == '\r' && data[pos + 1] == '\n') pos += 2;
    }
    return 0;
}

// Deliberately hand-rolled instead of strncasecmp/strcasestr/strtok_r:
// this file can't be compile-tested here (no devkitARM in this sandbox),
// and those aren't guaranteed present in every newlib config, so stick to
// the small set of libc calls that definitely are (strstr, strncmp, ...).
static bool ascii_case_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

static bool starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s || !ascii_case_eq(*s, *prefix)) return false;
        s++; prefix++;
    }
    return true;
}

static bool contains_ci(const char *haystack, const char *needle) {
    size_t needleLen = strlen(needle);
    if (needleLen == 0) return true;
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needleLen && p[i] && ascii_case_eq(p[i], needle[i])) i++;
        if (i == needleLen) return true;
    }
    return false;
}

static bool header_line_value(const char *line, const char *name, char *out, size_t outSize) {
    if (!starts_with_ci(line, name)) return false;
    const char *p = line + strlen(name);
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    snprintf(out, outSize, "%s", p);
    return true;
}

static Result do_single_request(HTTPC_RequestMethod method, const char *url,
                                 const HttpHeader *headers, int header_count,
                                 const u8 *body, u32 body_size,
                                 char *redirect_out, size_t redirect_out_size,
                                 HttpResponse *out) {
    ParsedUrl pu;
    if (!parse_url(url, &pu) || !pu.https) return HTTP_ERR_BAD_URL;

    TlsConn conn;
    Result rc = tls_connect(pu.host, pu.port, &conn);
    if (R_FAILED(rc)) return rc;

    // Build the request headers + body into one buffer.
    HttpBuffer req = {0};
    char line[1600];
    int n;

    n = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method_to_string(method), pu.path);
    rc = buffer_append(&req, (const u8 *)line, (u32)n);
    if (R_SUCCEEDED(rc)) {
        if (pu.port == (pu.https ? 443 : 80)) {
            n = snprintf(line, sizeof(line), "Host: %s\r\n", pu.host);
        } else {
            n = snprintf(line, sizeof(line), "Host: %s:%d\r\n", pu.host, pu.port);
        }
        rc = buffer_append(&req, (const u8 *)line, (u32)n);
    }
    if (R_SUCCEEDED(rc)) rc = buffer_append(&req, (const u8 *)"Connection: close\r\n", 20);
    if (R_SUCCEEDED(rc)) rc = buffer_append(&req, (const u8 *)"User-Agent: Konnect3DS\r\n", 25);
    if (R_SUCCEEDED(rc) && body && body_size > 0) {
        n = snprintf(line, sizeof(line), "Content-Length: %lu\r\n", (unsigned long)body_size);
        rc = buffer_append(&req, (const u8 *)line, (u32)n);
    }
    for (int i = 0; R_SUCCEEDED(rc) && i < header_count; i++) {
        n = snprintf(line, sizeof(line), "%s: %s\r\n", headers[i].name, headers[i].value);
        rc = buffer_append(&req, (const u8 *)line, (u32)n);
    }
    if (R_SUCCEEDED(rc)) rc = buffer_append(&req, (const u8 *)"\r\n", 2);
    if (R_SUCCEEDED(rc) && body && body_size > 0) rc = buffer_append(&req, body, body_size);

    if (R_FAILED(rc)) {
        free(req.data);
        tls_close(&conn);
        return rc;
    }

    int wret = tls_write_all(&conn.ssl, req.data, req.size);
    free(req.data);
    if (wret != 0) {
        tls_close(&conn);
        return mbedtls_to_result(wret);
    }

    // Read the whole response (we always send Connection: close, so
    // reading until EOF is enough -- no need to track Content-Length to
    // know when to stop).
    HttpBuffer raw = {0};
    u8 chunk[HTTP_RECV_CHUNK];
    for (;;) {
        int got = tls_read_some(&conn.ssl, chunk, sizeof(chunk));
        if (got > 0) {
            rc = buffer_append(&raw, chunk, (u32)got);
            if (R_FAILED(rc)) { free(raw.data); tls_close(&conn); return rc; }
            continue;
        }
        if (got == 0) break; // clean EOF
        free(raw.data);
        tls_close(&conn);
        return mbedtls_to_result(got);
    }
    tls_close(&conn);

    if (raw.size == 0) { free(raw.data); return HTTP_ERR_BAD_RESPONSE; }

    const u8 *sep = find_bytes(raw.data, raw.size, "\r\n\r\n");
    if (!sep) { free(raw.data); return HTTP_ERR_BAD_RESPONSE; }
    size_t headerLen = (size_t)(sep - raw.data);
    size_t bodyOffset = headerLen + 4;

    char *headerBlock = malloc(headerLen + 1);
    if (!headerBlock) { free(raw.data); return HTTP_ERR_OOM; }
    memcpy(headerBlock, raw.data, headerLen);
    headerBlock[headerLen] = '\0';

    unsigned int status = 0;
    sscanf(headerBlock, "HTTP/%*d.%*d %u", &status);

    char location[1024] = {0};
    bool chunked = false;
    bool haveContentLength = false;
    long contentLength = 0;

    // Walk header lines by hand (see header_line_value's comment for why).
    char *cursor = headerBlock;
    char *lineEnd = strstr(cursor, "\r\n");
    cursor = lineEnd ? lineEnd + 2 : cursor + strlen(cursor); // skip the status line

    while (*cursor) {
        lineEnd = strstr(cursor, "\r\n");
        size_t lineLen = lineEnd ? (size_t)(lineEnd - cursor) : strlen(cursor);

        char lineBuf[1024];
        size_t copyLen = lineLen < sizeof(lineBuf) - 1 ? lineLen : sizeof(lineBuf) - 1;
        memcpy(lineBuf, cursor, copyLen);
        lineBuf[copyLen] = '\0';

        char value[1024];
        if (header_line_value(lineBuf, "Location", value, sizeof(value))) {
            snprintf(location, sizeof(location), "%s", value);
        } else if (header_line_value(lineBuf, "Transfer-Encoding", value, sizeof(value))) {
            if (contains_ci(value, "chunked")) chunked = true;
        } else if (header_line_value(lineBuf, "Content-Length", value, sizeof(value))) {
            contentLength = strtol(value, NULL, 10);
            haveContentLength = true;
        }

        if (!lineEnd) break;
        cursor = lineEnd + 2;
    }
    free(headerBlock);

    if ((status >= 301 && status <= 303) || status == 307 || status == 308) {
        if (redirect_out && location[0]) {
            snprintf(redirect_out, redirect_out_size, "%s", location);
            out->status_code = status;
            free(raw.data);
            return HTTP_REDIRECT_SENTINEL;
        }
    }

    const u8 *bodyPtr = raw.data + bodyOffset;
    size_t bodyLen = raw.size - bodyOffset;

    HttpBuffer finalBody = {0};
    if (chunked) {
        rc = decode_chunked(bodyPtr, bodyLen, &finalBody);
        if (R_FAILED(rc)) { free(raw.data); return rc; }
    } else {
        if (haveContentLength && (size_t)contentLength < bodyLen) bodyLen = (size_t)contentLength;
        rc = buffer_append(&finalBody, bodyPtr, (u32)bodyLen);
        if (R_FAILED(rc)) { free(raw.data); return rc; }
    }
    free(raw.data);

    out->status_code = status;
    out->body = finalBody;
    return 0;
}

Result http_request(HTTPC_RequestMethod method, const char *url,
                     const HttpHeader *headers, int header_count,
                     const u8 *body, u32 body_size,
                     HttpResponse *out) {
    if (!s_httpInited) return HTTP_ERR_BAD_URL;
    memset(out, 0, sizeof(*out));

    char currentUrl[1024];
    snprintf(currentUrl, sizeof(currentUrl), "%s", url);

    for (int hop = 0; hop < HTTP_MAX_REDIRECTS; hop++) {
        char location[1024] = {0};
        Result rc = do_single_request(method, currentUrl, headers, header_count,
                                       body, body_size,
                                       location, sizeof(location), out);
        if (rc == HTTP_REDIRECT_SENTINEL) {
            snprintf(currentUrl, sizeof(currentUrl), "%s", location);
            continue;
        }
        return rc;
    }
    return HTTP_ERR_TOO_MANY_REDIRECTS;
}

Result http_request_file_body(HTTPC_RequestMethod method, const char *url,
                               const HttpHeader *headers, int header_count,
                               FILE *body_file, u32 body_size,
                               HttpResponse *out) {
    if (!s_httpInited) return HTTP_ERR_BAD_URL;
    memset(out, 0, sizeof(*out));

    u8 *buf = malloc(body_size);
    if (!buf) return HTTP_ERR_OOM;
    rewind(body_file);
    if (fread(buf, 1, body_size, body_file) != body_size) {
        free(buf);
        return HTTP_ERR_BAD_RESPONSE;
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

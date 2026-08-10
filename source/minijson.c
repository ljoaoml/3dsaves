#include "minijson.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// Finds `"key":` (with optional whitespace after the colon) anywhere in the
// buffer and returns a pointer just past it, or NULL.
static const char *find_value(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

bool json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *p = find_value(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case '"': out[o++] = '"'; break;
                case '\\': out[o++] = '\\'; break;
                case '/': out[o++] = '/'; break;
                default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

bool json_get_int(const char *json, const char *key, long *out) {
    const char *p = find_value(json, key);
    if (!p) return false;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

bool json_get_bool(const char *json, const char *key, bool *out) {
    const char *p = find_value(json, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

// Skips a JSON string literal starting at *p (which must point at the
// opening '"'), leaving *p just past the closing '"'. Shared by
// json_find_array()/json_array_next_object() so a '[', ']', '{' or '}'
// inside a string value (e.g. a file name containing a literal bracket)
// doesn't confuse their depth counting.
static void skip_string(const char **p) {
    (*p)++; // past opening '"'
    while (**p && **p != '"') {
        if (**p == '\\' && (*p)[1]) (*p)++;
        (*p)++;
    }
    if (**p == '"') (*p)++; // past closing '"'
}

const char *json_find_array(const char *json, const char *key, const char **arrayEnd) {
    const char *p = find_value(json, key);
    if (!p || *p != '[') return NULL;
    p++; // past '['

    const char *q = p;
    int depth = 1;
    while (*q && depth > 0) {
        if (*q == '"') { skip_string(&q); continue; }
        if (*q == '[') depth++;
        else if (*q == ']') { depth--; if (depth == 0) break; }
        q++;
    }
    if (depth != 0) return NULL; // malformed/truncated

    *arrayEnd = q;
    return p;
}

bool json_array_next_object(const char **cursor, const char *arrayEnd, char *out, size_t outSize) {
    if (outSize == 0) return false;

    const char *p = *cursor;
    while (p < arrayEnd && *p != '{') {
        if (*p == '"') { skip_string(&p); continue; }
        p++;
    }
    if (p >= arrayEnd) return false;

    const char *start = p;
    int depth = 0;
    while (p < arrayEnd) {
        if (*p == '"') { skip_string(&p); continue; }
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) { p++; break; }
        }
        p++;
    }
    if (depth != 0) return false; // malformed/truncated

    size_t len = (size_t)(p - start);
    size_t copyLen = len < outSize - 1 ? len : outSize - 1;
    memcpy(out, start, copyLen);
    out[copyLen] = '\0';

    *cursor = p;
    return true;
}

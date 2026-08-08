#pragma once
#include <stddef.h>
#include <stdbool.h>

// Deliberately not a general JSON parser: Dropbox's API returns small, flat,
// predictable JSON objects for the calls we make (token exchange, simple
// upload/list results), so a tiny string-search extractor avoids pulling in
// a full JSON dependency we can't test-compile here. If this project grows
// to parse nested structures (e.g. full recursive folder listings), swap
// this out for a real parser (cJSON) instead of extending it.

// Extracts a top-level string value for "key":"value" (handles \" \\ \/ \n
// \t \r escapes). Returns false if not found or buffer too small.
bool json_get_string(const char *json, const char *key, char *out, size_t out_size);

// Extracts a top-level numeric value for "key":123.
bool json_get_int(const char *json, const char *key, long *out);

// Extracts a top-level boolean value for "key":true/false.
bool json_get_bool(const char *json, const char *key, bool *out);

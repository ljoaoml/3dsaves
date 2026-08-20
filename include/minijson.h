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

// Finds a top-level array value for "key":[ ... ], returning a pointer to
// the first character inside the brackets and writing a pointer to the
// matching ']' into *arrayEnd (bracket/string-nesting aware, so a ']'
// inside a nested array or a string value doesn't end the scan early).
// Returns NULL (leaving *arrayEnd untouched) if the key isn't found, its
// value isn't an array, or the array is malformed/truncated.
const char *json_find_array(const char *json, const char *key, const char **arrayEnd);

// Iterates flat JSON objects inside an array located by json_find_array().
// On the first call, *cursor should be that function's return value; this
// advances *cursor itself afterward, so callers just loop until it returns
// false. Each call copies the next "{...}" element (braces included) into
// `out` -- still just a flat buffer, so nested objects/arrays inside an
// element are preserved verbatim rather than parsed, and json_get_string()/
// json_get_int() work on it exactly as they would on any other JSON
// object. Returns false once there are no more objects before `arrayEnd`.
bool json_array_next_object(const char **cursor, const char *arrayEnd, char *out, size_t outSize);

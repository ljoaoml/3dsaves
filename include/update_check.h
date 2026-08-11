#pragma once
#include <stdbool.h>
#include <stddef.h>

// Best-effort "is a newer build available" check (PKSM-style), comparing
// this build's embedded git commit (KONNECT3DS_GIT_HASH, set by the
// Makefile) against the tip of UPDATE_CHECK_BRANCH on GitHub via the
// repo's REST API. There's no packaged .cia release channel for this
// project yet -- just an actively developed branch rebuilt straight from
// a Codespace -- so a commit hash stands in for a version number here.
//
// On success, *hasUpdate reports whether the two differ and
// latestShortOut (if non-NULL) is filled with the first 7 chars of the
// remote commit's SHA (matching how KONNECT3DS_GIT_HASH itself is
// derived: `git rev-parse --short HEAD`) for display.
//
// Returns false (leaving *hasUpdate untouched) on any failure -- offline,
// DNS, TLS, GitHub's rate limit -- since "couldn't check" isn't the same
// as "up to date" and callers shouldn't report it as either.
bool update_check_run(bool *hasUpdate, char *latestShortOut, size_t latestShortOutSize);

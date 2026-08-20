#include "update_check.h"
#include "http.h"
#include "minijson.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// Tracks whatever branch actually gets rebuilt right now (see the repo's
// Codespaces devcontainer workflow) -- there's no merged "main" release
// for this WIP UI rewrite yet. Update this if the branch is renamed, or
// point it at the default branch once this lands there for good.
#define UPDATE_CHECK_BRANCH "claude/citro2d-ui-redesign"
#define UPDATE_CHECK_URL "https://api.github.com/repos/ljoaoml/3dsaves/commits/" UPDATE_CHECK_BRANCH

#ifndef KONNECT3DS_GIT_HASH
#define KONNECT3DS_GIT_HASH "unknown"
#endif

bool update_check_run(bool *hasUpdate, char *latestShortOut, size_t latestShortOutSize) {
    // GitHub's REST API rejects requests with no User-Agent (http.c
    // already sends one unconditionally) and wants this Accept header for
    // its documented JSON response shape rather than a media-type-specific
    // one.
    HttpHeader headers[] = {
        {"Accept", "application/vnd.github+json"},
    };

    HttpResponse resp;
    Result rc = http_request(HTTPC_METHOD_GET, UPDATE_CHECK_URL, headers, 1, NULL, 0, &resp);
    if (R_FAILED(rc)) return false;

    bool ok = false;
    if (resp.status_code == 200 && resp.body.data) {
        char sha[64] = {0};
        if (json_get_string((const char *)resp.body.data, "sha", sha, sizeof(sha)) && strlen(sha) >= 7) {
            if (latestShortOut && latestShortOutSize > 0) {
                size_t n = latestShortOutSize - 1 < 7 ? latestShortOutSize - 1 : 7;
                memcpy(latestShortOut, sha, n);
                latestShortOut[n] = '\0';
            }

            // KONNECT3DS_GIT_HASH may carry a "-dirty" suffix (see the
            // Makefile) -- truncating into a 7-char buffer drops it the
            // same way it drops any chars past the short hash itself.
            char local[8] = {0};
            snprintf(local, sizeof(local), "%s", KONNECT3DS_GIT_HASH);

            if (hasUpdate) {
                *hasUpdate = strcmp(local, "unknown") != 0 && strncmp(local, sha, 7) != 0;
            }
            ok = true;
        }
    }

    http_response_free(&resp);
    return ok;
}

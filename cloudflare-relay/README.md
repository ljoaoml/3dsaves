# Konnect3DS OAuth relay

A tiny Cloudflare Worker that lets the 3DS log in to Dropbox **without
typing the long authorization code by hand, and without the 3DS ever
making a network request to this relay**.

## Why this exists

Dropbox's OAuth flow normally redirects the browser that approved the
login back to your app (`redirect_uri`). That works fine when the app and
the browser are the same device, but here the browser runs on a *phone*
and the app runs on the *3DS* -- a redirect can't jump devices on its own.

On top of that, real-hardware testing found that the 3DS's own HTTPS
requests get a raw `400 Bad Request` straight from Cloudflare's edge on
*every* Cloudflare-fronted domain tried (`*.workers.dev`, a dedicated
custom domain, even an unrelated third-party site) -- while the exact
same TLS client has no trouble at all talking to Dropbox's own API
(`api.dropboxapi.com`). So a design where the 3DS polls this Worker over
the network, like most OAuth relays would, can never work here.

Instead this Worker does the **entire** OAuth exchange itself:

1. The 3DS shows a QR code for `<this>/start`.
2. The phone scans it; `/start` generates a PKCE pair and a random
   `state`, stores the verifier in KV keyed by `state`, and redirects to
   Dropbox's real login page.
3. The user logs in and approves; Dropbox redirects the phone back to
   `/callback` with an authorization code.
4. `/callback` looks up the stored verifier and exchanges the code for
   Dropbox tokens itself (Cloudflare -> Dropbox is a normal
   server-to-server request, not subject to the block above), then shows
   a page with a **Download** button that builds a small text file
   (client-side, no extra request) containing the finished
   `access_token` / `refresh_token` / `expires_at`, already in the exact
   format `source/auth.c` reads for its token file.
5. The user copies that *file* (not its contents) onto the SD card, e.g.
   via FTP, to `3ds/Konnect3DS/paste_tokens.txt`. The 3DS notices it
   automatically within about half a second and finishes logging in --
   no typing, no on-device network request to this relay at all.

See the comments in `src/index.js` for the security rationale (short
version: the PKCE verifier never leaves this Worker's KV store, and the
downloaded file is the same sensitive artifact the on-device token file
already is -- don't leave it lying around longer than it takes to copy
it over).

## One-time setup

Requires a (free) Cloudflare account and [`wrangler`](https://developers.cloudflare.com/workers/wrangler/install-and-update/)
(`npm install -g wrangler`).

```sh
cd cloudflare-relay
wrangler login

# Create the KV namespace used to hold in-flight PKCE verifiers:
wrangler kv namespace create CODES
```

That last command prints an `id`. Paste it into `wrangler.toml`, replacing
`REPLACE_WITH_YOUR_KV_NAMESPACE_ID`:

```toml
kv_namespaces = [
  { binding = "CODES", id = "PASTE_THE_ID_HERE" }
]
```

Then deploy:

```sh
wrangler deploy
```

This prints your Worker's URL, something like:

```
https://konnect3ds-oauth-relay.<your-subdomain>.workers.dev
```

That URL only ever needs to be reachable by the phone's browser (the
3DS never contacts it directly), so `*.workers.dev` works fine as-is --
a custom domain is optional, not required for correctness.

### Wire it into the app

1. In the [Dropbox App Console](https://www.dropbox.com/developers/apps),
   open your app -> **Settings** -> **OAuth 2** -> **Redirect URIs**, and
   add:
   ```
   https://<your worker's domain>/callback
   ```
   (exact match required, including `/callback` -- Dropbox does not
   support wildcards here).
2. In `include/auth.h`, set `RELAY_BASE_URL` to that same URL (without a
   trailing slash), e.g.:
   ```c
   #define RELAY_BASE_URL "https://konnect3ds-oauth-relay.yoursubdomain.workers.dev"
   ```
   or pass it at build time like `DROPBOX_CLIENT_ID`:
   ```sh
   CFLAGS+=' -DRELAY_BASE_URL=\"https://...\"' make
   ```
3. Rebuild and reinstall the app.

## Redeploying after changes

```sh
cd cloudflare-relay
wrangler deploy
```

## Cost

Cloudflare Workers' free tier (100,000 requests/day) and Workers KV free
tier are both far more than a personal-use app like this will ever need.

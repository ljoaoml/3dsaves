# 3dsaves OAuth relay

A tiny Cloudflare Worker that lets the 3DS log in to Dropbox **without
typing the authorization code by hand**.

## Why this exists

Dropbox's OAuth flow normally redirects the browser that approved the
login back to your app (`redirect_uri`). That works fine when the app and
the browser are the same device, but here the browser runs on a *phone*
and the app runs on the *3DS* -- a redirect can't jump devices on its own.

This Worker stands in the middle: Dropbox redirects the phone here, this
stores the resulting code for a few minutes (keyed by a random `state`
value the 3DS generated), and the 3DS polls this Worker until the code
shows up. See the comments in `src/index.js` for why this doesn't weaken
security (short version: PKCE means the code alone, without the verifier
that never leaves the 3DS, can't be redeemed for a token).

## One-time setup

Requires a (free) Cloudflare account and [`wrangler`](https://developers.cloudflare.com/workers/wrangler/install-and-update/)
(`npm install -g wrangler`).

```sh
cd cloudflare-relay
wrangler login

# Create the KV namespace used to hold in-flight codes:
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
https://3dsaves-oauth-relay.<your-subdomain>.workers.dev
```

### Wire it into the app

1. In the [Dropbox App Console](https://www.dropbox.com/developers/apps),
   open your app -> **Settings** -> **OAuth 2** -> **Redirect URIs**, and
   add:
   ```
   https://3dsaves-oauth-relay.<your-subdomain>.workers.dev/callback
   ```
   (exact match required, including `/callback` -- Dropbox does not
   support wildcards here).
2. In `include/auth.h`, set `RELAY_BASE_URL` to your Worker's URL
   (without a trailing slash), e.g.:
   ```c
   #define RELAY_BASE_URL "https://3dsaves-oauth-relay.your-subdomain.workers.dev"
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

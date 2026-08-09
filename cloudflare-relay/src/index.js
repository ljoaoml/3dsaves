// Konnect3DS OAuth relay
//
// The 3DS's TLS client gets a raw 400 straight from Cloudflare's edge on
// every request to this Worker (workers.dev, a dedicated custom domain,
// Bot Fight Mode on or off -- see the 3dsaves repo README for the full
// investigation), even though the exact same client talks to Dropbox's
// own servers (api.dropboxapi.com, content.dropboxapi.com) just fine. So
// the 3DS can never successfully poll this relay over the network for
// anything -- the whole OAuth exchange has to happen here, server-side,
// and the *result* (final Dropbox tokens, not an intermediate
// authorization code) has to reach the 3DS some other way: as a
// downloadable file the user grabs on their phone and copies onto the
// SD card (e.g. via FTP), no typing and no 3DS-side network request
// required at all.
//
// Flow:
//   1. 3DS shows a QR code for <this>/start.
//   2. Phone scans it; /start generates a PKCE pair + random state,
//      stores the verifier in KV keyed by state, and 302s to Dropbox's
//      real authorize URL (redirect_uri = <this>/callback).
//   3. User logs in to Dropbox and approves; Dropbox redirects the phone
//      back to /callback with the authorization code.
//   4. /callback looks up the stored verifier, exchanges the code for
//      tokens directly with Dropbox (Cloudflare -> Dropbox, a normal
//      server-to-server request -- Cloudflare itself isn't blocked,
//      only the 3DS's own requests to *this* relay are), and shows a
//      page with a "Download" link that builds a small text file
//      (client-side, a data: URI, no extra request) containing the
//      final access_token/refresh_token/expires_at in the exact format
//      source/auth.c already reads for its token file. The 3DS just
//      needs that file copied to the SD card; no parsing beyond what
//      auth_load_tokens() already does.
//
// Security: the PKCE verifier lives only in KV, single-use, expires
// quickly, and is never exposed in a URL or shown to the user. The
// final tokens do leave this relay in the callback page/download --
// unavoidable, since the whole point is getting them to the 3DS without
// it making a network request -- so treat the downloaded file exactly
// like the token file it becomes: don't leave it lying around longer
// than it takes to copy it to the SD card.

const PKCE_TTL_SECONDS = 900; // 15 minutes -- generous headroom for a real first-time login
const DROPBOX_CLIENT_ID = "xxd9vkjyoedihll"; // same public App key as include/auth.h
const DROPBOX_AUTHORIZE_URL = "https://www.dropbox.com/oauth2/authorize";
const DROPBOX_TOKEN_URL = "https://api.dropboxapi.com/oauth2/token";

function base64url(bytes) {
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function randomToken(byteLen) {
  const bytes = new Uint8Array(byteLen);
  crypto.getRandomValues(bytes);
  return base64url(bytes);
}

async function pkceChallenge(verifier) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier));
  return base64url(new Uint8Array(digest));
}

function htmlPage(title, message, ok, extraHtml) {
  const color = ok ? "#1a7f37" : "#cf222e";
  return `<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${title}</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  display:flex;align-items:center;justify-content:center;min-height:100vh;
  margin:0;background:#f6f8fa;color:#1f2328;text-align:center;padding:24px}
.card{max-width:420px}
h1{color:${color};font-size:1.3em}
</style></head>
<body><div class="card"><h1>${title}</h1><p>${message}</p>${extraHtml || ""}</div></body></html>`;
}

async function handleStart(url, env) {
  const verifier = randomToken(32); // 43 chars
  const challenge = await pkceChallenge(verifier);
  const state = randomToken(24); // 32 chars

  await env.CODES.put(`pkce:${state}`, verifier, { expirationTtl: PKCE_TTL_SECONDS });

  const redirectUri = `${url.origin}/callback`;
  const authorizeUrl = new URL(DROPBOX_AUTHORIZE_URL);
  authorizeUrl.searchParams.set("client_id", DROPBOX_CLIENT_ID);
  authorizeUrl.searchParams.set("response_type", "code");
  authorizeUrl.searchParams.set("code_challenge", challenge);
  authorizeUrl.searchParams.set("code_challenge_method", "S256");
  authorizeUrl.searchParams.set("token_access_type", "offline");
  authorizeUrl.searchParams.set("redirect_uri", redirectUri);
  authorizeUrl.searchParams.set("state", state);

  return Response.redirect(authorizeUrl.toString(), 302);
}

async function handleCallback(url, env) {
  const state = url.searchParams.get("state");
  const code = url.searchParams.get("code");
  const error = url.searchParams.get("error_description") || url.searchParams.get("error");

  if (error) {
    return new Response(htmlPage("Login cancelled", error, false), {
      headers: { "content-type": "text/html; charset=utf-8" },
    });
  }
  if (!state || !code) {
    return new Response(htmlPage("Missing information", "This link is missing required information -- go back to your 3DS and try again.", false), {
      status: 400,
      headers: { "content-type": "text/html; charset=utf-8" },
    });
  }

  const verifier = await env.CODES.get(`pkce:${state}`);
  if (!verifier) {
    return new Response(htmlPage("Link expired", "This login link expired or was already used -- go back to your 3DS and scan a fresh QR code.", false), {
      status: 400,
      headers: { "content-type": "text/html; charset=utf-8" },
    });
  }
  await env.CODES.delete(`pkce:${state}`); // single-use

  const redirectUri = `${url.origin}/callback`;
  const body = new URLSearchParams({
    grant_type: "authorization_code",
    code,
    client_id: DROPBOX_CLIENT_ID,
    code_verifier: verifier,
    redirect_uri: redirectUri,
  });

  const tokenResp = await fetch(DROPBOX_TOKEN_URL, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: body.toString(),
  });
  const tokenJson = await tokenResp.json();

  if (!tokenResp.ok || !tokenJson.access_token || !tokenJson.refresh_token) {
    const reason = tokenJson.error_description || tokenJson.error || `HTTP ${tokenResp.status}`;
    return new Response(htmlPage("Dropbox login failed", reason, false), {
      headers: { "content-type": "text/html; charset=utf-8" },
    });
  }

  // Cloudflare's clock is reliable (unlike the 3DS's -- see the repo
  // README for that whole story), so compute the absolute expiry here
  // rather than leaving it to the console.
  const expiresAt = Math.floor(Date.now() / 1000) + (tokenJson.expires_in || 14400) - 60;

  // Exact format source/auth.c's auth_load_tokens() already parses --
  // the 3DS doesn't need to understand anything new, just save this
  // file as-is to become its token file.
  const fileContent =
    `access_token=${tokenJson.access_token}\n` +
    `refresh_token=${tokenJson.refresh_token}\n` +
    `expires_at=${expiresAt}\n`;

  const downloadHtml = `
    <p>Download this file and copy it (as a file, not its contents) to
    <code>3ds/Konnect3DS/paste_tokens.txt</code> on the 3DS's SD card --
    it's picked up automatically within a second, no typing needed.</p>
    <a href="data:text/plain;charset=utf-8,${encodeURIComponent(fileContent)}"
       download="paste_tokens.txt"
       style="display:inline-block;margin:12px 0;padding:12px 20px;
       background:#0061fe;color:#fff;border-radius:6px;
       text-decoration:none;font-weight:600">Download paste_tokens.txt</a>`;

  return new Response(
    htmlPage("Almost done", "One more step to get this onto your 3DS:", true, downloadHtml),
    { headers: { "content-type": "text/html; charset=utf-8" } }
  );
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/start") {
      return handleStart(url, env);
    }
    if (url.pathname === "/callback") {
      return handleCallback(url, env);
    }
    return new Response("Konnect3DS OAuth relay -- see github.com/ljoaoml/3dsaves\n", {
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  },
};

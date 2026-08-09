# Bundled root CA certificates

These are public root CA certificates (DER-encoded), used at runtime so
HTTPS requests to Dropbox's API domains -- and now also the Cloudflare
relay's `*.workers.dev` domain, see `cloudflare-relay/` -- verify
correctly even though the 3DS's built-in trusted cert list
(`SSLC_DefaultRootCert_*` in libctru) is old and does not cover every
modern CA/chain. `source/http.c` loads all of these at startup and adds
them as trusted roots on every HTTPS request via `httpcAddTrustedRootCA`.

The Google Trust Services / USERTrust / Amazon roots were added after the
original Dropbox-only set turned out not to be enough once a second
domain (the relay) entered the picture -- HTTPS to it was failing
silently (the 3DS just never got a response) because none of the
originally bundled roots covered whatever CA issues `*.workers.dev`
certs. Lesson: prefer bundling broad CA coverage over narrowly matching
just the one domain you tested against.

Extracted from curl's [Mozilla CA bundle](https://curl.se/docs/caextract.html)
(`cacert.pem`) on 2026-08-08 and converted to DER with:

```
openssl x509 -in <cert>.pem -inform PEM -out <cert>.der -outform DER
```

| File | Subject CN |
|---|---|
| `isrg_root_x1.der` | ISRG Root X1 (Let's Encrypt) |
| `digicert_global_root_g2.der` | DigiCert Global Root G2 |
| `digicert_global_root_g3.der` | DigiCert Global Root G3 |
| `digicert_tls_rsa4096_root_g5.der` | DigiCert TLS RSA4096 Root G5 |
| `digicert_tls_ecc_p384_root_g5.der` | DigiCert TLS ECC P384 Root G5 |
| `gts_root_r1.der` | GTS Root R1 (Google Trust Services) |
| `gts_root_r4.der` | GTS Root R4 (Google Trust Services) |
| `usertrust_rsa.der` | USERTrust RSA Certification Authority (Sectigo/Comodo) |
| `amazon_root_ca_1.der` | Amazon Root CA 1 |

Dropbox has announced ([dropbox.tech, "API server root certificate changes
coming in 2026"](https://dropbox.tech/developers/api-server-certificate-changes))
that `api.dropboxapi.com` / `content.dropboxapi.com` / `www.dropbox.com`
are moving to a new issuing root at some point in 2026, without naming the
exact root in advance. That's exactly the failure mode this bundle-instead-
of-pin approach is meant to survive: if uploads/logins suddenly start
failing with a TLS/handshake error, the fix is to re-run the extraction
above against a fresh `cacert.pem` and add whatever new root Dropbox is
using, rather than chasing a single pinned certificate.

# Bundled root CA certificates

These are public root CA certificates (DER-encoded), used at runtime so
HTTPS requests to Dropbox's API domains -- and now also the Cloudflare
relay's `*.workers.dev` domain, see `cloudflare-relay/` -- verify
correctly even though the 3DS's built-in trusted cert list
(`SSLC_DefaultRootCert_*` in libctru) is old and does not cover every
modern CA/chain. `source/http.c` loads all of these at startup and adds
them as trusted roots on every HTTPS request via `httpcAddTrustedRootCA`.

The non-Dropbox roots were added chasing one HTTPS failure to the
Cloudflare relay's `*.workers.dev` (see `cloudflare-relay/`), decodable
as `0xD8A0A03C` = module HTTP, "invalid state", TLS certificate
verification failed. It took three rounds to actually fix, which is worth
recording so it isn't repeated:

1. First guess: Google Trust Services roots. Wrong -- didn't fix it.
2. Checked [crt.sh](https://crt.sh) for the actual cert in use: issued by
   **SSL.com** ("Cloudflare TLS Issuing ECC CA 4"). Added SSL.com's roots.
   Still didn't fully fix it -- Cloudflare turned out to be *rotating
   between two different issuers* for the same hostname (confirmed via
   [certspotter.com](https://api.certspotter.com)'s issuance history:
   alternating Google Trust Services and SSL.com certs, days apart).
3. Walked the Google side's actual AIA chain (fetching each issuer cert's
   own "CA Issuers" URL with curl+openssl, not guessing): Google Trust
   Services' `WE1` intermediate is itself signed by **GlobalSign ECC Root
   CA - R4**, not any of the "GTS Root" certs. That was the missing piece.

Lesson: bundle broad CA coverage instead of narrowly matching just the
one domain you tested against; a domain can rotate between multiple
issuers; and when a chain doesn't validate, walk the *actual* AIA chain
(each cert's issuer field / "CA Issuers" URL) rather than trusting
general knowledge of who a company "usually" uses for its root -- that
knowledge goes stale, and intermediate providers change hands.

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
| `sslcom_root_rsa.der` | SSL.com Root Certification Authority RSA |
| `sslcom_root_ecc.der` | SSL.com Root Certification Authority ECC |
| `sslcom_tls_rsa_root_2022.der` | SSL.com TLS RSA Root CA 2022 |
| `sslcom_tls_ecc_root_2022.der` | SSL.com TLS ECC Root CA 2022 |
| `globalsign_ecc_root_r4.der` | GlobalSign ECC Root CA - R4 (the actual root behind Google Trust Services' WE1) |
| `globalsign_ecc_root_r5.der` | GlobalSign ECC Root CA - R5 |
| `globalsign_root_r3.der` | GlobalSign Root CA - R3 |
| `globalsign_root_r6.der` | GlobalSign Root CA - R6 |

Dropbox has announced ([dropbox.tech, "API server root certificate changes
coming in 2026"](https://dropbox.tech/developers/api-server-certificate-changes))
that `api.dropboxapi.com` / `content.dropboxapi.com` / `www.dropbox.com`
are moving to a new issuing root at some point in 2026, without naming the
exact root in advance. That's exactly the failure mode this bundle-instead-
of-pin approach is meant to survive: if uploads/logins suddenly start
failing with a TLS/handshake error, the fix is to re-run the extraction
above against a fresh `cacert.pem` and add whatever new root Dropbox is
using, rather than chasing a single pinned certificate.

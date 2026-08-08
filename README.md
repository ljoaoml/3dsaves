# 3dsaves

A Nintendo 3DS homebrew app that backs up a game's save data to Dropbox.

Runs as a `.3dsx` via the Homebrew Launcher (no CFW-specific access
required — the same permission level used by tools like
[JKSM](https://github.com/J-D-K/JKSM) / [Checkpoint](https://github.com/BernardoGiordano/Checkpoint)
is enough to read another title's save archive).

## Status

This is a from-scratch scaffold: the code is written to the documented
libctru/Dropbox APIs but has **not been compiled or run on hardware or in
an emulator** in this session (no devkitARM toolchain / 3DS available
here). Treat it as a strong starting point that needs a build-and-test
pass, not as verified-working code. See "Known rough edges" below.

## What it does

1. Lists installed titles (SD + inserted cartridge) that look like regular
   games (`source/saves.c`).
2. Reads the selected title's `SAVEDATA` archive recursively and packs it
   into a `.zip` (stored, no compression) on the SD card
   (`source/saves.c`, `source/minizip_writer.c`).
3. Logs in to Dropbox using OAuth 2.0 Authorization Code + PKCE, with the
   *no-redirect-URI* variant Dropbox supports for apps that can't receive
   an HTTP redirect: the authorization code is shown on the Dropbox
   consent page for the user to copy and paste back in via the 3DS
   software keyboard (`source/auth.c`). No client secret is embedded in
   the binary.
4. Uploads the zip to `/3dsaves/<PRODUCT-CODE>_<TITLEID>.zip` in the
   user's Dropbox via `POST /2/files/upload` (`source/dropbox.c`).

Networking goes through libctru's `httpc` service (`source/http.c`), which
handles TLS itself — no raw sockets / `soc:u` needed. A handful of modern
root CA certs are bundled in `romfs/certs/` and trusted at runtime,
because the console's built-in cert list is dated. See
`romfs/certs/README.md` for why and how to refresh them.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`3ds-dev` package group (devkitARM + libctru):

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make
```

Produces `3dsaves.3dsx` (+ `.smdh`). Copy it to `/3ds/3dsaves/` on the SD
card and launch via Homebrew Launcher.

## One-time setup: register a Dropbox app

1. Go to the [Dropbox App Console](https://www.dropbox.com/developers/apps)
   and create an app:
   - API: **Scoped access**
   - Access type: **App folder** (simplest — keeps it to `/Apps/3dsaves`)
     or **Full Dropbox** if you'd rather control the path yourself.
   - Under **Permissions**, enable `files.content.write` and
     `files.content.read`.
2. Copy the app's **App key**.
3. Put it in `include/auth.h`, replacing `PUT_YOUR_DROPBOX_APP_KEY_HERE`
   in `DROPBOX_CLIENT_ID` — or pass it at build time instead:
   ```sh
   CFLAGS+=' -DDROPBOX_CLIENT_ID=\"your_app_key\"' make
   ```
   No app secret is needed (PKCE flow).

## Using it

- On first run, select **Log in to Dropbox**, open the printed URL on
  another device, approve access, then press A on the 3DS and paste the
  code Dropbox shows you.
- Pick a title from the list and it backs up + uploads immediately.
- Titles are listed by product code and title ID (e.g. `CTR-P-AREE`)
  rather than a friendly game name — resolving the SMDH icon/title info
  would need reading the NCCH header, which is a reasonable follow-up
  (see below).

## Known rough edges / follow-ups

- **Not build-tested.** The APIs used (`httpc`, `am`, `fs`, `ps`, `swkbd`)
  were cross-checked against current libctru headers, but there's no
  substitute for actually compiling with devkitARM and running it —
  please treat the first build as a debugging pass, not a rubber stamp.
- **Auth URL has no QR code.** The Dropbox authorize URL (with the PKCE
  `code_challenge` in the query string) is long — typing it by hand on a
  phone is tedious. Rendering it as an on-screen QR code (e.g. with
  [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator)'s
  C port + citro2d for pixel drawing instead of the plain text console
  used here) would fix this and is the highest-value UX improvement.
- **`EXTDATA`-only titles aren't handled**, only `SAVEDATA`
  (`ARCHIVE_USER_SAVEDATA`). Some titles keep save data in extdata
  instead/as well; adding that archive type is a moderate amount of
  additional, less-common code.
- **Large saves are fully buffered in RAM** for both zipping and upload
  (`httpc`'s raw POST API wants the whole body up front). Fine for
  typical save sizes; would need chunked upload handling for very large
  extdata.
- **ZIP entries use STORE (no compression).** Simple and easy to verify
  by hand; swapping in DEFLATE (devkitPro's `zlib` portlib) would shrink
  uploads if that ever matters.
- **One provider (Dropbox).** Google Drive and OneDrive were the other
  candidates discussed; both support a real OAuth device-code flow (nicer
  than Dropbox's copy/paste code), so if a second provider gets added,
  start there and give `auth.c`/`dropbox.c` a shared interface first.
- **No restore path yet** — this only backs up. Restoring would mean the
  reverse: download from Dropbox, unzip, and write files back into the
  `SAVEDATA` archive, then `FSUSER_ControlArchive` commit if the archive
  needs it (double-check this against Checkpoint's restore code).

## Project layout

```
source/    application code (.c)
include/   headers (.h)
romfs/     bundled read-only assets (root CA certs)
Makefile   devkitARM build rules
```

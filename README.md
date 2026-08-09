# 3dsaves

A Nintendo 3DS homebrew app that backs up a game's save data to Dropbox.

Builds as either a `.3dsx` (run via Homebrew Launcher, no extra tools) or
a `.cia` (install with FBI, get a HOME menu icon like a real app). Reading
another title's save archive needs the same elevated FS access tools like
[JKSM](https://github.com/J-D-K/JKSM) / [Checkpoint](https://github.com/BernardoGiordano/Checkpoint)
rely on — see the CIA permission note under "Known rough edges" below.

## Status

Compiles clean (`.3dsx`) as of the latest fixes below. The `.cia` build
path was written against real working templates (Checkpoint's RSF, the
classic `3ds-template` Makefile) but hasn't actually been run through
`make cia` yet — see "Known rough edges" for the one part of it
(the FS permission list) that most needs on-hardware verification.

Fixes applied while getting the first build green (kept here so it's
obvious what changed since the initial scaffold, not because the code
still needs work):
- `ARCH` flags updated for current devkitARM/GCC (`-mtype=thumb
  -mthumb-interwork` were removed upstream; now `-mtp=soft`).
- Missing `#include <stdio.h>` / `<stddef.h>` in `include/http.h` /
  `include/dropbox.h` (used `FILE`/`size_t` without including them).
- `_3DSXDEPS` was referenced but never defined, so the `.smdh` icon never
  actually got built before `3dsxtool` tried to embed it.

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
card and launch via Homebrew Launcher — **`.3dsx` is not something FBI
installs**, it just runs directly from that launcher.

### Building a `.cia` instead (installs with FBI)

Needs two extra tools that aren't part of the base `3ds-dev` group:

```sh
(dkp-)pacman -S general-tools   # provides makerom + bannertool
make cia
```

Produces `3dsaves.cia`. Copy it to the SD card and install it with FBI
like any other CIA — it'll get a real HOME menu icon (a plain generated
placeholder icon/banner for now, see `resources/`).

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

- **CIA permission list is the least-verified part.** `resources/template.rsf`
  declares `CategorySystemApplication` in `FileSystemAccess` specifically so
  a properly-installed CIA (not run through the Homebrew Launcher exploit,
  which already has broad FS access regardless of any RSF) can open other
  titles' `SAVEDATA` archives. Whether this is sufficient depends partly on
  what your CFW (e.g. Luma3DS) patches system-wide — if `.cia` save backup
  fails with a permission error on real hardware, this list is the first
  place to check.
- **`.3dsx` end-to-end still needs a real backup+upload test.** It compiles
  and should run/list titles, but a full "pick a title, upload, check
  Dropbox" pass hasn't been confirmed on hardware yet.
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
source/     application code (.c)
include/    headers (.h)
romfs/      bundled read-only assets (root CA certs), embedded in both
            the .3dsx (via 3dsxtool --romfs) and the .cia (via the RSF's
            RomFs/RootPath)
resources/  CIA-only: AppInfo (title/author/ids), template.rsf
            (permissions), icon.png/banner.png/audio.wav (placeholder art)
Makefile    devkitARM build rules (`make` -> .3dsx, `make cia` -> .cia)
```
